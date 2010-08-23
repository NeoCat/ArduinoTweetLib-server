//  Copyright (C) 2010 Georg Kaindl
//  http://gkaindl.com
//
//  This file is part of Arduino EthernetDNS.
//
//  EthernetDNS is free software: you can redistribute it and/or modify
//  it under the terms of the GNU Lesser General Public License as
//  published by the Free Software Foundation, either version 3 of
//  the License, or (at your option) any later version.
//
//  EthernetDNS is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU Lesser General Public License for more details.
//
//  You should have received a copy of the GNU Lesser General Public
//  License along with EthernetDNS. If not, see
//  <http://www.gnu.org/licenses/>.
//

#include <string.h>
#include <stdlib.h>

extern "C" {
   #include "wiring.h"
   
   #include "../Ethernet/utility/types.h"
   #include "../Ethernet/utility/socket.h"
   #include "../Ethernet/utility/spi.h"
   #include "../Ethernet/utility/w5100.h"
   
   #include "utility/EthernetDnsUtil.h"
}

#include "EthernetDNS.h"

// this is one of Google's public DNS servers
#define  DEFAULT_DNS_SERVER      {8, 8, 8, 8}

#define  DNS_SERVER_PORT         (53)
#define  DNS_CLIENT_PORT         (1234)

#define  DNS_TIMEOUT_MILLIS      (10000)  // 10 seconds

#define  NUM_SOCKETS             (4)

#undef _USE_MALLOC_

typedef struct _DNSHeader_t {
   uint16_t    xid;
   uint8_t     recursionDesired:1;
   uint8_t     truncated:1;
   uint8_t     authoritiveAnswer:1;
   uint8_t     opCode:4;
   uint8_t     queryResponse:1;
   uint8_t     responseCode:4;
   uint8_t     checkingDisabled:1;
   uint8_t     authenticatedData:1;
   uint8_t     zReserved:1;
   uint8_t     recursionAvailable:1;
   uint16_t    queryCount;
   uint16_t    answerCount;
   uint16_t    authorityCount;
   uint16_t    additionalCount;
} __attribute__((__packed__)) DNSHeader_t;

typedef enum _DNSOpCode_t {
   DNSOpQuery     = 0,
   DNSOpIQuery    = 1,
   DNSOpStatus    = 2,
   DNSOpNotify    = 4,
   DNSOpUpdate    = 5
} DNSOpCode_t;

EthernetDNSClass::EthernetDNSClass()
{
   memset(&this->_dnsData, 0, sizeof(DNSDataInternal_t));
   
   uint8_t defaultDnsIp[] = DEFAULT_DNS_SERVER;
   memcpy(this->_dnsData.serverIpAddr, defaultDnsIp, sizeof(this->_dnsData.serverIpAddr));
   
   this->_lastSendMillis = 0;
   this->_state = DNSStateIdle;
   this->_socket = -1;
}

EthernetDNSClass::~EthernetDNSClass()
{
   (void)this->_closeDNSSession();
}

void EthernetDNSClass::setDNSServer(const byte dnsServerIpAddr[4])
{
   if (NULL != dnsServerIpAddr)
      memcpy(this->_dnsData.serverIpAddr, (const char*)dnsServerIpAddr, 4);
}

// return values:
// 1 on success
// 0 otherwise
int EthernetDNSClass::_startDNSSession()
{
   (void)this->_closeDNSSession();
   
   int i;
   for (i = NUM_SOCKETS-1; i>=0; i--)
      if (SOCK_CLOSED == IINCHIP_READ(Sn_SR(i))) {
         if (socket(i, Sn_MR_UDP, DNS_CLIENT_PORT, 0) > 0) {
            this->_socket = i;
            break;
         }
      }

   if (this->_socket < 0)
      return 0;
	
	uint16 port = DNS_SERVER_PORT;

   for (i=0; i<4; i++)
      IINCHIP_WRITE((Sn_DIPR0(this->_socket) + i), this->_dnsData.serverIpAddr[i]);

   IINCHIP_WRITE(Sn_DPORT0(this->_socket), (uint8)((port & 0xff00) >> 8));
   IINCHIP_WRITE((Sn_DPORT0(this->_socket) + 1), (uint8)(port & 0x00ff));
   
   return 1;
}

// return values:
// 1 on success
// 0 otherwise
int EthernetDNSClass::_closeDNSSession()
{
   if (this->_socket > -1)
      close(this->_socket);

   this->_socket = -1;
}

// return value:
// A DNSError_t (DNSSuccess on success, something else otherwise)
// in "int" mode: positive on success, negative on error
DNSError_t EthernetDNSClass::sendDNSQuery(const char* hostName)
{
   DNSError_t statusCode = DNSSuccess;
   
   if (this->_state != DNSStateIdle) {
      statusCode = DNSAlreadyProcessingQuery;
   } else {
      if (this->_startDNSSession() < 0) {
         statusCode = DNSSocketError;
      } else {
         statusCode = this->_sendDNSQueryPacket(hostName);
      }
   }
   
   return statusCode;
}

// return value:
// A DNSError_t (DNSSuccess on success, something else otherwise)
// in "int" mode: positive on success, negative on error
DNSError_t EthernetDNSClass::_sendDNSQueryPacket(const char* hostName)
{
   DNSError_t statusCode = DNSSuccess;
   uint16_t ptr = 0;
#if defined(_USE_MALLOC_)
   DNSHeader_t* dnsHeader = NULL;
#else
   DNSHeader_t dnsHeaderBuf;
   DNSHeader_t* dnsHeader = &dnsHeaderBuf;
#endif
   const char* p1, *p2;
   char* p3;
   uint8_t* buf;
   int c, i, bsize, len;

   if (NULL == hostName || 0 == *hostName) {
      statusCode = DNSInvalidArgument;
      goto errorReturn;
   }
   
   ptr = IINCHIP_READ(Sn_TX_WR0(this->_socket));
 	ptr = ((ptr & 0x00ff) << 8) + IINCHIP_READ(Sn_TX_WR0(this->_socket) + 1);

#if defined(_USE_MALLOC_)
   dnsHeader = (DNSHeader_t*)malloc(sizeof(DNSHeader_t));
   if (NULL == dnsHeader) {
      statusCode = DNSOutOfMemory;
      goto errorReturn;
   }
#endif
          
   memset(dnsHeader, 0, sizeof(DNSHeader_t));
   
   dnsHeader->xid = dns_htons(++this->_dnsData.xid);
   dnsHeader->recursionDesired = 1;
   dnsHeader->queryCount = dns_htons(1);
   dnsHeader->opCode = DNSOpQuery;
   
   write_data(this->_socket, (vuint8*)dnsHeader, (vuint8*)ptr, sizeof(DNSHeader_t));
   ptr += sizeof(DNSHeader_t);
   
   p1 = hostName;
   bsize = sizeof(DNSHeader_t);
   buf = (uint8_t*)dnsHeader;
   while(*p1) {
      c = 1;
      p2 = p1;
      while (0 != *p2 && '.' != *p2) { p2++; c++; };

      p3 = (char*)buf;
      i = c;
      len = bsize-1;
      *p3++ = (uint8_t)--i;
      while (i-- > 0) {
         *p3++ = *p1++;
         
         if (--len <= 0) {
            write_data(this->_socket, (vuint8*)buf, (vuint8*)ptr, bsize);
            ptr += bsize;
            len = bsize;
            p3 = (char*)buf;
         }
      }
      
      while ('.' == *p1)
         ++p1;
      
      if (len != bsize) {
         write_data(this->_socket, (vuint8*)buf, (vuint8*)ptr, bsize-len);
         ptr += bsize-len;
      }
   }
      
   // first byte is the query string's zero termination, then qtype and class follow.
   buf[0] = buf[1] = buf[3] = 0;
   buf[2] = buf[4] = 1;

   write_data(this->_socket, (vuint8*)buf, (vuint8*)ptr, 5);
   ptr += 5;

   IINCHIP_WRITE(Sn_TX_WR0(this->_socket), (vuint8)((ptr & 0xff00) >> 8));
   IINCHIP_WRITE((Sn_TX_WR0(this->_socket) + 1), (vuint8)(ptr & 0x00ff));
   
   IINCHIP_WRITE(Sn_CR(this->_socket), Sn_CR_SEND);

   while(IINCHIP_READ(Sn_CR(this->_socket)));
   
   if (_state == DNSStateIdle) {
      this->_dnsData.lastQueryFirstXid = this->_dnsData.xid;
   }
   
   this->_state = DNSStateQuerySent;

   statusCode = DNSSuccess;

errorReturn:
   this->_lastSendMillis = millis();

#if defined(_USE_MALLOC_)
   if (NULL != dnsHeader)
      free(dnsHeader);
#endif

   return statusCode;
}

// return value:
// A DNSError_t (DNSSuccess on success, something else otherwise)
// in "int" mode: positive on success, negative on error
DNSError_t EthernetDNSClass::pollDNSReply(byte ipAddr[4])
{
   DNSError_t statusCode = DNSSuccess;
#if defined(_USE_MALLOC_)
   DNSHeader_t* dnsHeader = NULL;
#else
   DNSHeader_t dnsHeaderBuf;
   DNSHeader_t* dnsHeader = &dnsHeaderBuf;
#endif
   uint8_t* buf;
   uint32_t svr_addr;
   uint16_t svr_port, udp_len, ptr, qCnt, aCnt;
   
   if (DNSStateQuerySent != this->_state) {
      statusCode = DNSNothingToDo;
      goto errorReturn;
   }
   
   if (NULL == ipAddr) {
      statusCode = DNSInvalidArgument;
      goto errorReturn;
   }
   
   if (0 == (IINCHIP_READ(Sn_RX_RSR0(this->_socket))) &&
       0 == (IINCHIP_READ(Sn_RX_RSR0(this->_socket) + 1))) {
      statusCode = DNSTryLater;
      goto errorReturn;
   }

#if defined(_USE_MALLOC_)
   dnsHeader = (DNSHeader_t*)malloc(sizeof(DNSHeader_t));
   if (NULL == dnsHeader) {
      statusCode = DNSOutOfMemory;
      goto errorReturn;
   }
#endif
   
   ptr = IINCHIP_READ(Sn_RX_RD0(this->_socket));
   ptr = ((ptr & 0x00ff) << 8) + IINCHIP_READ(Sn_RX_RD0(this->_socket) + 1);

   // read UDP header
   buf = (uint8_t*)dnsHeader;
   read_data(this->_socket, (vuint8*)ptr, (vuint8*)buf, 8);
   ptr += 8;

   memcpy(&svr_addr, buf, sizeof(uint32_t));
   *((uint16_t*)&svr_port) = dns_ntohs(*((uint32_t*)(buf+4)));
   *((uint16_t*)&udp_len) = dns_ntohs(*((uint32_t*)(buf+6)));
   
   read_data(this->_socket, (vuint8*)ptr, (vuint8*)dnsHeader, sizeof(DNSHeader_t));
   
   if (0 != dnsHeader->responseCode) {
      if (3 == dnsHeader->responseCode)
         statusCode = DNSNotFound;
      else
         statusCode = DNSServerError;
      
      goto errorReturn;
   }
   
   dnsHeader->xid = dns_ntohs(dnsHeader->xid);
   qCnt = dns_ntohs(dnsHeader->queryCount);
   aCnt = dns_ntohs(dnsHeader->answerCount);
   
   if (dnsHeader->queryResponse &&
       DNSOpQuery == dnsHeader->opCode &&
       DNS_SERVER_PORT == svr_port &&
       this->_dnsData.lastQueryFirstXid <= dnsHeader->xid && this->_dnsData.xid >= dnsHeader->xid &&
       0 == memcmp(&svr_addr, this->_dnsData.serverIpAddr, sizeof(uint32_t))) {
          statusCode = DNSServerError; // if we don't find our A record answer, the server messed up.
                    
          int i, offset = sizeof(DNSHeader_t);
          uint8_t* buf = (uint8_t*)dnsHeader;
          int rLen;
          
          // read over the query section and answer section, stop on the first a record
          for (i=0; i<qCnt+aCnt; i++) {
             do {
                read_data(this->_socket, (vuint8*)(ptr+offset), (vuint8*)buf, 1);
                rLen = buf[0];
                if (rLen > 128) // handle DNS name compression
                   offset += 2;
                else
                   offset += rLen + 1;
             } while (rLen > 0 && rLen <= 128);
                          
             // if this is an answer record, there are more fields to it.
             if (i >= qCnt) {
                read_data(this->_socket, (vuint8*)(ptr+offset), (vuint8*)buf, 4);
                offset += 8; // skip over the 4-byte TTL field
                read_data(this->_socket, (vuint8*)(ptr+offset), (vuint8*)&buf[4], 2);
                offset += 2;
                
                // if class and qtype match, and data length is 4, this is an IP address, and
                // we're done.
                if (1 == buf[1] && 1 == buf[3] && 4 == buf[5]) {
                   read_data(this->_socket, (vuint8*)(ptr+offset), (vuint8*)ipAddr, 4);
                   statusCode = DNSSuccess;
                   break;
                }
             } else
                offset += 4; // eat the query type and class fields of a query
          }
   }
   
   ptr += udp_len;
   
   IINCHIP_WRITE(Sn_RX_RD0(this->_socket),(vuint8)((ptr & 0xff00) >> 8));
   IINCHIP_WRITE((Sn_RX_RD0(this->_socket) + 1),(vuint8)(ptr & 0x00ff));

   IINCHIP_WRITE(Sn_CR(this->_socket),Sn_CR_RECV);

   while(IINCHIP_READ(Sn_CR(this->_socket)));

errorReturn:

   if (DNSTryLater == statusCode) {
      unsigned long now = millis();
      if (now - this->_lastSendMillis > DNS_TIMEOUT_MILLIS || now < this->_lastSendMillis) {
         this->_state = DNSStateIdle;
         statusCode = DNSTimedOut;
      }
   } else {
      this->_closeDNSSession();
      this->_state = DNSStateIdle;
   }

#if defined(_USE_MALLOC_)
   if (NULL != dnsHeader)
      free(dnsHeader);
#endif

   return statusCode;
}

DNSError_t EthernetDNSClass::resolveHostName(const char* hostName, byte ipAddr[4])
{
   DNSError_t statusCode = this->sendDNSQuery(hostName);
   
   if (DNSSuccess == statusCode) {
      do {
         statusCode = this->pollDNSReply(ipAddr);
         
         if (DNSTryLater == statusCode)
            delay(10);
      } while (DNSTryLater == statusCode);
   }
   
   return statusCode;
}

EthernetDNSClass EthernetDNS;
