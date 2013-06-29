/*
 * Copyright (c) 2013, nilfs
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * * Neither the name of the <ORGANIZATION> nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#include <iostream>
#include <curl/curl.h>
#include <string>
#include <vector>
#include <atomic>
#include <map>
#include <chrono>
#include <functional>

// 通信完了時のコールバック
typedef std::function<void(const char*, size_t)> RequestCompleteCallback;

class HttpTransaction
{
public:
    HttpTransaction( const RequestCompleteCallback& callback )
    :m_Curl(nullptr)
    ,m_Callback(callback)
    ,m_Completed(false)
    {
        m_Curl = curl_easy_init();
    }
    
    ~HttpTransaction()
    {
        curl_easy_cleanup(m_Curl);
        m_Curl = nullptr;
    }
    
public:
    CURL* GetCurl() { return m_Curl; }
    
    void OnResponse( const char* data, size_t dataSize )
    {
        m_Callback( data, dataSize );
        
        m_Completed = true;
    }
    
    bool IsCompleted() const { return m_Completed; }
    
private:
    CURL* m_Curl;
    RequestCompleteCallback m_Callback;
    bool m_Completed;
};

class HttpRequest
{
public:
    enum RequestMethodType
    {
        GET,
        POST,
    };
    
public:
    HttpRequest( const char* url, RequestMethodType method )
    :m_Url(url)
    ,m_MethodType(method)
    {}
    
public:
    const char* GetUrl() const { return m_Url; }
    
private:
    // コピーするかどうか迷ったけど、一旦コピーしない形で
    const char* m_Url;
    RequestMethodType m_MethodType;
};

typedef unsigned int HttpHandle;

/**
 *  1回分のHTTP通信処理オブジェクト
 */
class HttpTransactionHandle
{
public:
    HttpTransactionHandle( const HttpHandle handle )
    :m_Handle(handle)
    {
    }
    
public:
    void SetCallback( std::function<void(char*, size_t)> callback )
    {
        m_Callback = callback;
    }
    
public:
    HttpHandle GetHandle() const{ return m_Handle; }
    
private:
    const HttpHandle m_Handle;
    std::function<void(char*, size_t)> m_Callback;
};


/**
 * Http通信をするクライアントクラス
 */
class HttpClient
{
public:
    HttpClient()
    :m_MultiHandle(nullptr)
    ,m_HandleCount(0)
    {
        m_MultiHandle = curl_multi_init();
    }
    
    ~HttpClient()
    {
        curl_multi_cleanup( m_MultiHandle );
    }
    
public:
    void Update()
    {
        curl_multi_perform(m_MultiHandle, &m_HandleCount);
        
        CURLMsg *msg = nullptr;
        int msgs_left = 0;
        while ((msg = curl_multi_info_read(m_MultiHandle, &msgs_left))) {
            // 特に使わないので捨てておく
        }
    }
    
    HttpTransactionHandle AddRequest( const HttpRequest& request, const RequestCompleteCallback& callback )
    {
        auto transaction = new HttpTransaction( callback );
     
        CURL* curl = transaction->GetCurl();
        curl_easy_setopt(curl, CURLOPT_URL, request.GetUrl() );
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &HttpClient::_OnResponse );
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, transaction );
        curl_multi_add_handle(m_MultiHandle, curl);
        
        auto handle = _CreateHandle();
        m_Handles[handle] = transaction;

        return HttpTransactionHandle( handle );
    }
        
public:
    bool IsCompleted( const HttpTransactionHandle& response )
    {
        HttpTransaction* transaction = _GetCurlHandle( response.GetHandle() );
        if( transaction )
        {
            return transaction->IsCompleted();
        }
        else
        {
            // 存在しないハンドルなので、既に終了して削除されている可能性がある
            return true;
        }
    }
    
    bool ReleaseTransaction( const HttpTransactionHandle& response )
    {
        HttpTransaction* transaction = _GetCurlHandle( response.GetHandle() );
        if( transaction )
        {
            m_Handles.erase( response.GetHandle() );
            delete transaction;

            return true;
        }
        else
        {
            return false;
        }
    }
    
private:
    static size_t _OnResponse(void *ptr, size_t size, size_t count, void *transaction) {

        const size_t dataSize = size*count;
        ((HttpTransaction*)transaction)->OnResponse((char*)ptr, dataSize);
        
        return dataSize;
    }


private:
    HttpHandle _CreateHandle()
    {
        HttpHandle dst;
        do
        {
            dst = m_Lock;
            dst = (dst + 1) % 0xffffffff; // オーバーフロー対策. 適当な数で折り返すように
        }
        while( std::atomic_exchange_explicit(&m_Lock, dst, std::memory_order_release) );
        
        return dst;
    }
    
    HttpTransaction* _GetCurlHandle( const HttpHandle& handle )
    {
        auto it = m_Handles.find( handle );
        
        if( it != m_Handles.end() )
        {
            return it->second;
        }
        else
        {
            return nullptr;
        }
    }
    
private:
    static std::atomic<HttpHandle> m_Lock;
    
private:
    CURLM* m_MultiHandle;
    std::map<HttpHandle, HttpTransaction*> m_Handles;
    int m_HandleCount; // 接続中のハンドル数
};

std::atomic<HttpHandle> HttpClient::m_Lock = ATOMIC_VAR_INIT(0U);

int main(int argc, const char * argv[])
{
    auto time_point = std::chrono::system_clock::now();
    
    HttpClient client;
    auto respone = client.AddRequest( HttpRequest( "http://google.co.jp", HttpRequest::GET ), []( const char* data, size_t dataSize ){
        
        std::cout << data << std::endl;
    } );
    
    while( !client.IsCompleted( respone ) )
    {
        client.Update();
    }
    
    client.ReleaseTransaction( respone );
    
    auto duration = std::chrono::system_clock::now() - time_point ;
    std::cout << duration.count() / 1000.0 / 1000.0 << std::endl ;

    return 0;
}

