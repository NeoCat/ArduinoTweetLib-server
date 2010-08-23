/*
  Twitter.cpp - Arduino library to Post messages to Twitter using OAuth.
  Copyright (c) NeoCat 2010. All right reserved.
  
  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. 
 */

#include "Twitter.h"

#define LIB_DOMAIN "arduino-tweet.appspot.com"
static uint8_t server[] = {0,0,0,0}; // IP address of LIB_DOMAIN

Twitter::Twitter(const char *token) : client(server, 80), token(token)
{
}

static int strlen(const char *msg)
{
	int i = 0;
	while (msg[i++]);
	return i-1;
}

bool Twitter::post(const char *msg)
{
	DNSError err = EthernetDNS.resolveHostName(LIB_DOMAIN, server);
	if (err != DNSSuccess) {
		return false;
	}
	parseStatus = 0;
	statusCode = 0;
	if (client.connect()) {
		client.println("POST http://" LIB_DOMAIN "/update HTTP/1.0");
		client.print("Content-Length: ");
		client.println(strlen(msg)+strlen(token)+14);
		client.println();
		client.print("token=");
		client.print(token);
		client.print("&status=");
		client.println(msg);
	} else {
		return false;
	}
	return true;
}

bool Twitter::checkStatus(void)
{
	if (!client.connected()) {
		client.flush();
		client.stop();
		return false;
	}
	if (!client.available())
		return true;
	char c = client.read();
	switch(parseStatus) {
	case 0:
		if (c == ' ') parseStatus++; break;  // skip "HTTP/1.1 "
	case 1:
		if (c >= '0' && c <= '9') {
			statusCode *= 10;
			statusCode += c - '0';
		} else {
			parseStatus++;
		}
	}
	return true;
}

int Twitter::wait(void)
{
	while (checkStatus());
	return statusCode;
}
