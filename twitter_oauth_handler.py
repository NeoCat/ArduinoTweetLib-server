# -*- coding: utf-8 -*-
"""
Twitter OAuth Support for Google App Engine Apps.

Using this in your app should be relatively straightforward:

* Edit the configuration section below with the CONSUMER_KEY and CONSUMER_SECRET
  from Twitter.

* Modify to reflect your App's domain and set the callback URL on Twitter to:

    http://your-app-name.appspot.com/oauth/twitter/callback

* Use the demo in ``MainHandler`` as a starting guide to implementing your app.

Note: You need to be running at least version 1.1.9 of the App Engine SDK.

-- 
I hope you find this useful, tav

"""

# Released into the Public Domain by tav@espians.com

import sys

from datetime import datetime, timedelta
from hashlib import sha1
from hmac import new as hmac
from os.path import dirname, join as join_path
from random import getrandbits
from time import time
from urllib import urlencode, quote as urlquote
from uuid import uuid4
from wsgiref.handlers import CGIHandler

sys.path.insert(0, join_path(dirname(__file__), 'lib')) # extend sys.path

from demjson import decode as decode_json

from google.appengine.api.urlfetch import fetch as urlfetch, GET, POST
from google.appengine.ext import db
from google.appengine.api import memcache
import webapp2 as webapp

# ------------------------------------------------------------------------------
# configuration -- SET THESE TO SUIT YOUR APP!!
# ------------------------------------------------------------------------------

OAUTH_APP_SETTINGS = {

    'twitter': {
        'consumer_key': '',
        'consumer_secret': '',

        'request_token_url': 'https://api.twitter.com/oauth/request_token',
        'access_token_url': 'https://api.twitter.com/oauth/access_token',
        'user_auth_url': 'https://api.twitter.com/oauth/authorize',

        'default_api_prefix': 'https://api.twitter.com/1.1',
        'default_api_suffix': '.json',
        },
    }

CLEANUP_BATCH_SIZE = 100
EXPIRATION_WINDOW = timedelta(seconds=60*60*1) # 1 hour

try:
    from config import OAUTH_APP_SETTINGS
except:
    pass

STATIC_OAUTH_TIMESTAMP = 12345 # a workaround for clock skew/network lag

# ------------------------------------------------------------------------------
# utility functions
# ------------------------------------------------------------------------------

def get_service_key(service, cache={}):
    if service in cache: return cache[service]
    return cache.setdefault(
        service, "%s&" % encode(OAUTH_APP_SETTINGS[service]['consumer_secret'])
        )

def create_uuid():
    return 'id-%s' % uuid4()

def encode(text):
    if isinstance(text, unicode):
        arg = str(text.encode('UTF-8'))
    else:
        arg = str(text)
    return urlquote(arg, '')

def twitter_specifier_handler(client):
    return client.get('/account/verify_credentials')['screen_name']

OAUTH_APP_SETTINGS['twitter']['specifier_handler'] = twitter_specifier_handler

class MyError(Exception):
    def __init__(self, code, value):
        self.code = code
        self.value = value
    def __str__(self):
        return repr(self.code) + '-' + repr(self.value)

# ------------------------------------------------------------------------------
# db entities
# ------------------------------------------------------------------------------

class OAuthRequestToken(db.Model):
    """OAuth Request Token."""

    oauth_token = db.StringProperty()
    oauth_token_secret = db.StringProperty()
    created = db.DateTimeProperty(auto_now_add=True)

class OAuthAccessToken(db.Model):
    """OAuth Access Token."""

    specifier = db.StringProperty()
    oauth_token = db.StringProperty()
    oauth_token_secret = db.StringProperty()
    created = db.DateTimeProperty(auto_now_add=True)

class Accounts(db.Model):
    """Old OAuth Access Token."""

    token = db.StringProperty()
    secret = db.StringProperty()
    date = db.DateTimeProperty(auto_now_add=True)

# ------------------------------------------------------------------------------
# oauth client
# ------------------------------------------------------------------------------

class OAuthClient(object):

    __public__ = ('callback', 'cleanup', 'login', 'logout')

    def __init__(self, service, handler, oauth_callback=None, **request_params):
        self.service = service
        self.service_info = OAUTH_APP_SETTINGS[service]
        self.service_key = None
        self.handler = handler
        self.request_params = request_params
        self.oauth_callback = oauth_callback
        self.token = None

    # public methods

    def set_token(self, token):

        secret = memcache.get(token)
        if secret is not None:
            self.token = OAuthAccessToken(oauth_token = token,
                                          oauth_token_secret = secret)
            return self.token
        oauth_token = OAuthAccessToken.all().filter('oauth_token = ', token).fetch(1)
        if not oauth_token:
            old_token = Accounts.all().filter('token =', token).fetch(1)
            if len(old_token) == 1:
                memcache.set(key=token, value=old_token[0].secret)
                self.token = OAuthAccessToken(oauth_token = old_token[0].token,
                                              oauth_token_secret = old_token[0].secret)
                return self.token
            return None
        self.token = oauth_token[0]
        memcache.set(key=token, value=self.token.oauth_token_secret)
        return self.token

    def get(self, api_method, http_method='GET', expected_status=(200,), **extra_params):

        if not (api_method.startswith('http://') or api_method.startswith('https://')):
            api_method = '%s%s%s' % (
                self.service_info['default_api_prefix'], api_method,
                self.service_info['default_api_suffix']
                )

        if self.token is None:
            raise MyError(500, "Error - token is not set")

        fetch = urlfetch(self.get_signed_url(
            api_method, self.token, http_method, **extra_params
            ))

        if fetch.status_code not in expected_status:
            raise MyError(fetch.status_code, fetch.content)

        return decode_json(fetch.content)

    def post(self, api_method, http_method='POST', expected_status=(200,), **extra_params):

        if not (api_method.startswith('http://') or api_method.startswith('https://')):
            api_method = '%s%s%s' % (
                self.service_info['default_api_prefix'], api_method,
                self.service_info['default_api_suffix']
                )

        if self.token is None:
            raise MyError(500, "token is not set")

        fetch = urlfetch(url=api_method, payload=self.get_signed_body(
            api_method, self.token, http_method, **extra_params
            ), method=http_method)

        if fetch.status_code not in expected_status:
            raise MyError(fetch.status_code, fetch.content)

        return decode_json(fetch.content)

    def login(self):

        return self.get_request_token()

    def logout(self, return_to='/'):
        self.expire_cookie()
        self.handler.redirect(self.handler.request.get("return_to", return_to))

    # oauth workflow

    def get_request_token(self):

        token_info = self.get_data_from_signed_url(
            self.service_info['request_token_url'], **self.request_params
            )

        try:
            token = OAuthRequestToken(
                **dict(token.split('=') for token in token_info.split('&'))
            )
        except:
            self.handler.error(500)
            return "Error - Request token is invalid! [" + token_info + "]"

        # token.put()
        memcache.set('request_token:'+token.oauth_token, token.oauth_token_secret, 300)

        if self.oauth_callback:
            oauth_callback = {'oauth_callback': self.oauth_callback}
        else:
            oauth_callback = {}

        self.handler.redirect(self.get_signed_url(
            self.service_info['user_auth_url'], token, **oauth_callback
            ))

    def callback(self):

        write = self.handler.response.out.write
        oauth_token = self.handler.request.get("oauth_token")
        oauth_verifier = self.handler.request.get("oauth_verifier")

        if not oauth_token:
            return get_request_token()

        #oauth_token = OAuthRequestToken.all().filter(
        #    'oauth_token =', oauth_token).fetch(1)[0]
        secret = memcache.get('request_token:'+oauth_token)
        if secret is None:
            self.handler.error(500)
            write("Error - Request token is invalid! [" + oauth_token + "]")
            return ""
        oauth_token = OAuthRequestToken(oauth_token=oauth_token, oauth_token_secret = secret)

        token_info = self.get_data_from_signed_url(
            self.service_info['access_token_url'], oauth_token,
            oauth_verifier = oauth_verifier
            )

        self.token = OAuthAccessToken(
            **dict(token.split('=') for token in token_info.split('&'))
            )
        memcache.set(key=self.token.oauth_token, value=self.token.oauth_token_secret)

        if 'specifier_handler' in self.service_info:
            specifier = self.token.specifier = self.service_info['specifier_handler'](self)
            old = OAuthAccessToken.all().filter(
                'specifier =', specifier)
            db.delete(old)

        #db.delete(oauth_token)
        memcache.delete('request_token:'+oauth_token.oauth_token)

        self.token.put()
        self.expire_cookie()
        write('<html><head><title>Arduino Tweet Lib - token</title><link rel="stylesheet" type="text/css" href="/style.css" charset="UTF-8"></head>')
        write('<body><h3>Your token is:</h3><div id="emp">')
        write(self.token.oauth_token)
        write('</div></body>')
        write('</html>')
        return ""

    def cleanup(self):
        #query = OAuthRequestToken.all().filter(
        #    'created <', datetime.now() - EXPIRATION_WINDOW
        #    )
        #count = query.count(CLEANUP_BATCH_SIZE)
        #db.delete(query.fetch(CLEANUP_BATCH_SIZE))
        #return "Cleaned %i entries" % count
        return "Not supported"

    # request marshalling

    def get_data_from_signed_url(self, __url, __token=None, __meth='GET', **extra_params):
        return urlfetch(self.get_signed_url(
            __url, __token, __meth, **extra_params
            )).content

    def get_signed_url(self, __url, __token=None, __meth='GET',**extra_params):
        return '%s?%s'%(__url, self.get_signed_body(__url, __token, __meth, **extra_params))

    def get_signed_body(self, __url, __token=None, __meth='GET',**extra_params):

        service_info = self.service_info

        kwargs = {
            'oauth_consumer_key': service_info['consumer_key'],
            'oauth_signature_method': 'HMAC-SHA1',
            'oauth_version': '1.0',
            'oauth_timestamp': int(time()),
            'oauth_nonce': getrandbits(64),
            }

        kwargs.update(extra_params)

        if self.service_key is None:
            self.service_key = get_service_key(self.service)

        if __token is not None:
            kwargs['oauth_token'] = __token.oauth_token
            key = self.service_key + encode(__token.oauth_token_secret)
        else:
            key = self.service_key

        message = '&'.join(map(encode, [
            __meth.upper(), __url, '&'.join(
                '%s=%s' % (encode(k), encode(kwargs[k])) for k in sorted(kwargs)
                )
            ]))

        kwargs['oauth_signature'] = hmac(
            key, message, sha1
            ).digest().encode('base64')[:-1]

        return urlencode(dict([k, v.encode('utf-8') if isinstance(v, unicode) else v] for k, v in kwargs.items()))

    # who stole the cookie from the cookie jar?

    def get_cookie(self):
        return self.handler.request.cookies.get(
            'oauth.%s' % self.service, ''
            )

    def set_cookie(self, value, path='/'):
        self.handler.response.headers.add_header(
            'Set-Cookie', 
            '%s=%s; path=%s; expires="Fri, 31-Dec-2021 23:59:59 GMT"' %
            ('oauth.%s' % self.service, value, path)
            )

    def expire_cookie(self, path='/'):
        self.handler.response.headers.add_header(
            'Set-Cookie', 
            '%s=; path=%s; expires="Fri, 31-Dec-1999 23:59:59 GMT"' %
            ('oauth.%s' % self.service, path)
            )

# ------------------------------------------------------------------------------
# oauth handler
# ------------------------------------------------------------------------------

class OAuthHandler(webapp.RequestHandler):

    def get(self, service, action=''):

        if service not in OAUTH_APP_SETTINGS:
            return self.response.out.write(
                "Unknown OAuth Service Provider: %r" % service
                )

        client = OAuthClient(service, self)

        if action in client.__public__:
            self.response.out.write(getattr(client, action)())
        else:
            self.response.out.write(client.login())

# ------------------------------------------------------------------------------
# modify this demo MainHandler to suit your needs
# ------------------------------------------------------------------------------

HEADER = """
  <html><head><title>Arduino Tweet Lib</title></head><body>
  """

FOOTER = "</body></html>"

class MainHandler(webapp.RequestHandler):

    def get(self):

        client = OAuthClient('twitter', self)

        write = self.response.out.write; write(HEADER)

        if not client.get_cookie():
            write('<a href="/oauth/twitter/login">Login via Twitter</a>')
            write(FOOTER)
            return

        write('<a href="/oauth/twitter/logout">Logout from Twitter</a><br /><br />')

        info = client.get('/account/verify_credentials')

        write("<strong>Screen Name:</strong> %s<br />" % info['screen_name'])
        write("<strong>Location:</strong> %s<br />" % info['location'])

        rate_info = client.get('/account/rate_limit_status')

        write("<strong>API Rate Limit Status:</strong> %r" % rate_info)

        write(FOOTER)
        return ""


class UpdateHandler(webapp.RequestHandler):

    def get(self):

        client = OAuthClient('twitter', self)
        
        write = self.response.out.write

        token = self.request.get('token')
        if not token:
            self.error(403)
            write('Error 403 - token is not specified')
            return
        
        status = self.request.get('status')
        if not status:
            self.error(403)
            write('Error 403 - status is not specified')
            return
        
        access_token = client.set_token(token)
        if not access_token:
            self.error(403)
            write('Error 403 - token is invalid')
            return

        d = datetime.today()
        qtime = str(int(d.strftime('%s'))/600)
        qkey = 'quota:'+qtime+":"+token
        quota = memcache.incr(qkey)
        if quota is None:
            memcache.add(qkey, 1, 600)
        elif quota > 50:
            self.error(503)
            write("Error 503 - Too many access")
            return

        try:
            result = client.post('/statuses/update', status = status)
            write('OK')
        except MyError, e:
            try:
                msg = decode_json(e.value)
                self.error(e.code)
                write("Error %d - %s\n" % (e.code, msg['error']))
            except:
                self.error(e.code)
                write("Error %d - %s\n" % (e.code, e.value))

    def post(self):
        return self.get()

# ------------------------------------------------------------------------------
# Application specification
# ------------------------------------------------------------------------------

app = webapp.WSGIApplication([
       ('/oauth/(.*)/(.*)', OAuthHandler),
       ('/', MainHandler),
       ('/update', UpdateHandler)
       ], debug=True)

