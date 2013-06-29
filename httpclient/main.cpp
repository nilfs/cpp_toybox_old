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
#include <thread>
#include <mutex>

#define ARRAY_SIZEOF( array ) ( sizeof(array)/sizeof(array[0]) )

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
    ,m_PostField(nullptr)
    {}
    
public:
    void SetPostField( const char* field ){ m_PostField = field; }
    
    const char* GetUrl() const { return m_Url; }
    const char* GetPostField() const { return m_PostField; }
    RequestMethodType GetMethodType() const { return m_MethodType; }
    
private:
    // コピーするかどうか迷ったけど、一旦コピーしない形で
    const char* m_Url;
    const char* m_PostField;
    RequestMethodType m_MethodType;
};

/**
 *  1回分のHTTP通信処理オブジェクト
 */
class HttpTransactionHandle
{
public:
    typedef unsigned int HandleId;
    
public:
    // 不正なハンドルID
    static const HandleId INVALID_HANDLE_ID = UINT32_MAX;
    
public:
    // 不要なハンドルを返す
    static const HttpTransactionHandle& Invalid()
    {
        static HttpTransactionHandle s_Invalid;
        return s_Invalid;
    }
    
public:
    HttpTransactionHandle( const HandleId handle=INVALID_HANDLE_ID )
    :m_Handle(handle)
    {
    }
    
    HttpTransactionHandle( const HttpTransactionHandle&& handle )
    :m_Handle(handle.m_Handle)
    {
    }
    
public:
    HttpTransactionHandle& operator=( const HttpTransactionHandle& handle )
    {
        m_Handle = handle.m_Handle;
        
        return *this;
    }
    
public:
    HandleId GetHandleId() const{ return m_Handle; }
    
    bool IsInvalid() const { return m_Handle == INVALID_HANDLE_ID; }
    bool IsValid()   const { return m_Handle != INVALID_HANDLE_ID; }
    
private:
    HandleId m_Handle;
};

std::mutex g_mutex;

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
        std::lock_guard<std::mutex> lock(g_mutex);

        auto transaction = new HttpTransaction( callback );
     
        CURL* curl = transaction->GetCurl();
        curl_easy_setopt(curl, CURLOPT_URL, request.GetUrl() );
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &HttpClient::_OnResponse );
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, transaction );
        
        switch( request.GetMethodType() )
        {
            case HttpRequest::POST:
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.GetPostField());
                break;
                
            case HttpRequest::GET:
            default:
                break;
        }
        
        auto result = curl_multi_add_handle(m_MultiHandle, curl);
        
        auto handle = _CreateHandle();
        m_Handles[handle] = transaction;
        
        return HttpTransactionHandle( handle );
    }
        
public:
    bool IsCompleted( const HttpTransactionHandle& handle )
    {
        HttpTransaction* transaction = _GetCurlHandle( handle.GetHandleId() );
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
    
    bool ReleaseTransaction( const HttpTransactionHandle& handle )
    {
        HttpTransaction* transaction = _GetCurlHandle( handle.GetHandleId() );
        if( transaction )
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            m_Handles.erase( handle.GetHandleId() );
            delete transaction;
            return true;
        }
        else
        {
            return false;
        }
    }
    
private:
    // 受信完了したときのコールバック関数
    static size_t _OnResponse(void *ptr, size_t size, size_t count, void *transaction) {

        const size_t dataSize = size*count;
        ((HttpTransaction*)transaction)->OnResponse((char*)ptr, dataSize);
        
        return dataSize;
    }

private:
    HttpTransactionHandle::HandleId _CreateHandle()
    {
        std::atomic_fetch_add(&m_Lock, (HttpTransactionHandle::HandleId)1);
        return m_Lock;
    }
    
    HttpTransaction* _GetCurlHandle( const HttpTransactionHandle::HandleId& handle )
    {
        std::lock_guard<std::mutex> lock(g_mutex);

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
    static std::atomic<HttpTransactionHandle::HandleId> m_Lock;
    
private:
    CURLM* m_MultiHandle;
    std::map<HttpTransactionHandle::HandleId, HttpTransaction*> m_Handles;
    int m_HandleCount; // 接続中のハンドル数
};

std::atomic<HttpTransactionHandle::HandleId> HttpClient::m_Lock = ATOMIC_VAR_INIT(0U);

int main(int argc, const char * argv[])
{
    // GET Methodのテスト
    HttpClient client;
    {
        auto time_point = std::chrono::system_clock::now();
        
        HttpTransactionHandle handles[10];
        
        std::atomic<int> val(0);
        for( int i=0; i<ARRAY_SIZEOF(handles); ++i )
        {
            auto thread = std::thread( [&val, &handles, &client, i](){
                handles[i] = client.AddRequest( HttpRequest( "http://google.co.jp", HttpRequest::GET ), [&val]( const char* data, size_t dataSize ){
                    
                    std::atomic_fetch_add(&val, 1);
                    std::cout << "val : " << val << std::endl;
                    std::cout << data << std::endl;
                } );
            } );
            thread.detach();
        }
        
        while( true )
        {
            client.Update();
            
            bool allCompleted = true;
            
            for( int i=0; i<ARRAY_SIZEOF(handles); ++i )
            {
                if( !client.IsCompleted( handles[i] ) )
                {
                    allCompleted = false;
                }
            }
            
            if( allCompleted )
            {
                break;
            }
        }
        
        for( int i=0; i<ARRAY_SIZEOF(handles); ++i )
        {
            client.ReleaseTransaction( handles[i] );
        }
        
        auto duration = std::chrono::system_clock::now() - time_point ;
        std::cout << "google.co.jpにGETするのにかかった時間" << duration.count() / 1000.0 / 1000.0 << std::endl ;
    }
    
    // POSTメソッドのテスト。google.co.jpはpostには対応していないのでエラーが返ってくる
    {
        HttpRequest request( "http://google.co.jp", HttpRequest::POST );
        request.SetPostField("name=hoge");
        auto handle = client.AddRequest( request, []( const char* data, size_t dataSize ){
        
            std::cout << data << std::endl;
            
        });
        
        while( !client.IsCompleted(handle) )
        {
            client.Update();
        }
    }
    return 0;
}

