/*
  Twitter.cpp - Arduino library to Post messages to Twitter using OAuth.
  Copyright (c) NeoCat 2010. All right reserved.
  
  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. 
*/

// ver1.2 - Use <Udp.h> to support IDE 0019 or later

#ifndef TWITTER_H
#define TWITTER_H

#include <inttypes.h>
#include <avr/pgmspace.h>
#include <SPI.h>
#include <Ethernet.h>
#include <Udp.h>
#include <EthernetDNS.h>

class Twitter
{
private:
	uint8_t parseStatus;
	int statusCode;
	const char *token;
	Client client;
public:
	Twitter(const char *user_and_passwd);
	
	bool post(const char *msg);
	bool checkStatus(void);
	int  wait(void);
	int  status(void) { return statusCode; }
};

#endif	//TWITTER_H
