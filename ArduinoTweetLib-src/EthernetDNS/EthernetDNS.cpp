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

// 2010-Oct-24  Modified by NeoCat :
//   Use Udp library included in Arduino IDE 0019 or later.

#include <string.h>
#include <stdlib.h>
#include "EthernetDNS.h"
extern "C" {
   #include "wiring.h"
}

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
}

void EthernetDNSClass::setDNSServer(const byte dnsServerIpAddr[4])
{
   if (NULL != dnsServerIpAddr)
      memcpy(this->_dnsData.serverIpAddr, (const char*)dnsServerIpAddr, 4);
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
      statusCode = this->_sendDNSQueryPacket(hostName);
   }
   
   return statusCode;
}

// return value:
// A DNSError_t (DNSSuccess on success, something else otherwise)
// in "int" mode: positive on success, negative on error
DNSError_t EthernetDNSClass::_sendDNSQueryPacket(const char* hostName)
{
   Udp.begin(DNS_CLIENT_PORT);
   
   DNSError_t statusCode = DNSSuccess;
#if defined(_USE_MALLOC_)
   DNSHeader_t* dnsHeader = NULL;
#else
   DNSHeader_t dnsHeader[4]; // [1-3] is buffer for data
#endif
   const char* p1, *p2;
   char* p3;
   uint8_t* buf;
   int c, i, bsize, len;

   if (NULL == hostName || 0 == *hostName) {
      statusCode = DNSInvalidArgument;
      goto errorReturn;
   }

#if defined(_USE_MALLOC_)
   dnsHeader = (DNSHeader_t*)malloc(sizeof(DNSHeader_t)*4);
   if (NULL == dnsHeader) {
      statusCode = DNSOutOfMemory;
      goto errorReturn;
   }
#endif
          
   memset(dnsHeader, 0, sizeof(DNSHeader_t)*4);
   
   dnsHeader->xid = dns_htons(++this->_dnsData.xid);
   dnsHeader->recursionDesired = 1;
   dnsHeader->queryCount = dns_htons(1);
   dnsHeader->opCode = DNSOpQuery;
   
   p1 = hostName;
   bsize = sizeof(DNSHeader_t);
   buf = (uint8_t*)&dnsHeader[1];
   p3 = (char*)buf;
   while(*p1) {
      c = 1;
      p2 = p1;
      while (0 != *p2 && '.' != *p2) { p2++; c++; };

      i = c;
      len = bsize-1;
      *p3++ = (uint8_t)--i;
      while (i-- > 0) {
         *p3++ = *p1++;
         
         if (--len <= 0) {
            len = bsize;
         }
      }
      
      while ('.' == *p1)
         ++p1;
   }
      
   // first byte is the query string's zero termination, then qtype and class follow.
   p3[0] = p3[1] = p3[3] = 0;
   p3[2] = p3[4] = 1;

   Udp.sendPacket((uint8_t*)dnsHeader, p3+5-(char*)dnsHeader,
                  this->_dnsData.serverIpAddr, DNS_SERVER_PORT);
   
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
   DNSError_t statusCode = DNSTryLater;
#if defined(_USE_MALLOC_)
   uint8_t* buf;
   DNSHeader_t* dnsHeader = NULL;
#else
   uint8_t buf[256];
   DNSHeader_t* dnsHeader = (DNSHeader_t*)buf;
#endif
   uint32_t svr_addr;
   uint16_t svr_port, udp_len, qCnt, aCnt;
   
   if (DNSStateQuerySent != this->_state) {
      statusCode = DNSNothingToDo;
      goto errorReturn;
   }
   
   if (NULL == ipAddr) {
      statusCode = DNSInvalidArgument;
      goto errorReturn;
   }
   
   udp_len = Udp.available();
   if (0 == udp_len) {
      statusCode = DNSTryLater;
      goto errorReturn;
   }

#if defined(_USE_MALLOC_)
   buf = (DNSHeader_t*)malloc(udp_len);
   dnsHeader = (uint8_t*)buf;
   if (NULL == dnsHeader) {
      statusCode = DNSOutOfMemory;
      goto errorReturn;
   }
#else
   udp_len = min(sizeof(buf), udp_len);
#endif
   
   // read UDP header
   Udp.readPacket(buf, udp_len, (uint8_t*)&svr_addr, &svr_port);
   
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
          int rLen;
          
          // read over the query section and answer section, stop on the first a record
          for (i=0; i<qCnt+aCnt; i++) {
             do {
                rLen = buf[offset];
                if (rLen > 128) // handle DNS name compression
                   offset += 2;
                else
                   offset += rLen + 1;
             } while (rLen > 0 && rLen <= 128 && offset < udp_len);
                          
             // if this is an answer record, there are more fields to it.
             if (i >= qCnt) {
                // if class and qtype match, and data length is 4, this is an IP address, and
                // we're done.
                memcpy((uint8_t*)ipAddr, buf+offset+16, 4);
                if (1 == buf[offset+1] && 1 == buf[offset+3] && 4 == buf[offset+9]) {
                   memcpy((uint8_t*)ipAddr, buf+offset+10, 4);
                   statusCode = DNSSuccess;
                   break;
                // skip CNAME record.
                } else if (5 == buf[offset+1]) {
                   offset += buf[offset+9] + 10;
                }
             } else
                offset += 4; // eat the query type and class fields of a query
             if (offset >= udp_len) break;
          }
   }
   
errorReturn:

   if (DNSTryLater == statusCode) {
      unsigned long now = millis();
      if (now - this->_lastSendMillis > DNS_TIMEOUT_MILLIS || now < this->_lastSendMillis) {
         this->_state = DNSStateIdle;
         statusCode = DNSTimedOut;
      }
   } else {
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
