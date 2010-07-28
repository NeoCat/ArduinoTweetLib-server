require 'rubygems'
require 'haml'
require 'json'
require 'sinatra'
require 'simple-oauth'
require 'uri'
require 'dm-core'

DataMapper.setup(:default, "appengine://auto")

helpers do
  include Rack::Utils
  alias_method :h, :escape_html
end

configure do
  use Rack::Session::Cookie
  CONSUMER_KEY = '<<< PUT YOUR CONSUMER KEY HERE >>>'
  CONSUMER_SECRET = '<<< PUT YOUR CONSUMER SECRET HERE >>>'
  PROVIDER = 'http://twitter.com'
  APIBASE = 'http://api.twitter.com/1'
  COUNT = 20
end

class Account
  include DataMapper::Resource
  property :id, Serial
  property :token, String
  property :secret, String
  property :date, Time, :default => lambda { |r, p| Time.now } # must be a Proc  
end


template :layout do
  <<-EOF
!!! XML
!!! Strict
 
%html
  %head
    %title Tweet Library for Arduino
    %meta{:"http-equiv"=>"Content-Type", :content=>"text/html", :charset=>"utf-8"}
    %link{:rel=>"stylesheet", :type=>"text/css", :href=>"/style.css"}
  %body
    != yield
EOF
end

def simple_oauth
  SimpleOAuth.new(CONSUMER_KEY, CONSUMER_SECRET, @token, @token_secret)
end

def base_url
  default_port = (request.scheme == "http") ? 80 : 443
  port = (request.port == default_port) ? "" : ":#{request.port.to_s}"
  "#{request.scheme}://#{request.host}#{port}"
end

error do
  'Error - ' + request.env['sinatra.error'].name
end

get '/request_token' do
  callback = "#{base_url}/access_token"
  @token = ''
  @token_secret = ''
  request_token_url = PROVIDER + '/oauth/request_token'
  response = simple_oauth.request_token(request_token_url, callback)
  session[:request_token] = response[:token]
  session[:request_token_secret] = response[:secret]
  redirect response[:authorize]
end

get '/access_token' do
  @token = session[:request_token]
  @token_secret = session[:request_token_secret]
  access_token_url = PROVIDER + '/oauth/access_token'
  response = simple_oauth.access_token(access_token_url, params[:oauth_verifier])
  session[:request_token] = ''
  session[:request_token_secret] = ''
  account = Account.first(:token => response[:token])
  account = Account.new(:token => response[:token]) unless account
  account[:secret] = response[:secret]
  account.save
  haml <<-EOF
Your account is created successfully.
%div#emp
  %big Your token : #{account[:token]}
Put this token into your sketch.
EOF
end

get '/update' do
  haml <<-EOF
%form{:method => 'POST'}
  status :
  %input{:name => 'status'}
  %br
  token :
  %input{:name => 'token'}
  %br
  %input{:type => 'submit'}
EOF
end

post '/update' do
  unless params[:token]
    status 403
    return haml 'token is not given'
  end
  account = Account.first(:token => params[:token])
  unless account
    status 403
    return haml 'Incorrect token'
  end
  @token = account[:token]
  @token_secret = account[:secret]
  status = URI.escape(params[:status], /[^-_.a-zA-Z\d]/n)
  update_url = APIBASE + "/statuses/update.json?status=#{status}"
  response = simple_oauth.post(update_url)
  unless response.code.to_i == 200
    status response.code
    return haml "Error #{response.code}\n%br\nstatus=#{status}"
  end
  haml 'OK'
end

=begin
get '/' do
  haml <<-EOF
<a href="/request_token">Get account using OAuth</a>
EOF
end

get '/timeline' do
  return haml 'token is not given' unless params[:token]
  account = Account.first(:token => params[:token])
  return haml 'Incorrect token' unless account
  @token = account[:token]
  @token_secret = account[:secret]
  timeline_url = APIBASE + "/statuses/friends_timeline.json?count=#{COUNT}"
  response = simple_oauth.get(timeline_url)
  return haml "Error #{response.code}" unless response.code.to_i == 200
  @timeline = JSON(response.body)
  haml <<-EOF
%dl
 - @timeline.each do |status|
  %dt= status['user']['screen_name']
  %dd= status['text']
EOF
end

get '/debug/list' do
  @accounts = Account.all
  haml <<-EOF
%dl
 - @accounts.each do |account|
  %dt= account[:id]
  %dd= account[:date]
  %dd= account[:token]
EOF
end
=end
