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
#include <functional>
#include <vector>
#include <cassert>
#include <atomic>

/**
 * std::tupleを関数の引数にする実装をここから持ってきた http://d.hatena.ne.jp/redboltz/20110811/1313024577
 * std::functionに対応するために少し変更してある
 */
template <class F, class... Ts, class... Us>
typename std::enable_if<
sizeof...(Us) == sizeof...(Ts),
typename F::result_type>::type
apply_impl(F& fun, std::tuple<Ts...>& args, Us*... us)
{
    return fun(*us...);
}

template <class F, class... Ts, class... Us>
typename std::enable_if<
sizeof...(Us) < sizeof...(Ts),
typename F::result_type>::type
apply_impl(F& fun, std::tuple<Ts...>& args, Us*... us)
{
    return apply_impl(fun, args, us..., &std::get<sizeof...(Us)>(args));
}

template <class F, class... Ts>
typename F::result_type
apply(F& fun, std::tuple<Ts...>& args)
{
    return apply_impl(fun, args);
}


template< typename ArgFirst, typename ...ArgTypes >
class RequestUpdate
{
private:
    typedef std::function< void(ArgFirst, ArgTypes...)> RequestExecuter;
    typedef std::tuple< ArgFirst, ArgTypes... > Parameter;
    
    typedef std::function< bool(Parameter, Parameter)> SortPredicator;

public:
    RequestUpdate()
    {
        m_Requests = &m_Requests1;
        m_UpdatingRequests = &m_Requests2;
    }
    
public:
    /**
     *  リクエストを処理する関数
     */
    void SetRequestExecuter( const RequestExecuter& executer )
    {
        m_Executer = executer;
    }
    
    
    /**
     *  リクエストパラメータの処理順番を決定するソート関数
     */
    void SetRequestSortPredicator( const SortPredicator& func )
    {
        m_SortPred = func;
    }
    
    void AddRequest( ArgFirst argFirst, ArgTypes... args )
    {
        std::lock_guard<std::mutex> lock( m_Mutex );
        
        m_Requests->emplace_back( std::make_tuple( argFirst, args... ) );
    }
    
    void Update()
    {
        {
            std::lock_guard<std::mutex> lock( m_Mutex );
            std::swap( m_Requests, m_UpdatingRequests );
        }

        std::sort(m_UpdatingRequests->begin(), m_UpdatingRequests->end(), m_SortPred);
        for( Parameter& param : *m_UpdatingRequests )
        {
            apply(m_Executer, param);
        }
        
        m_UpdatingRequests->clear();
    }

private:
    std::vector< Parameter > m_Requests1;   //リクエストのバッファ
    std::vector< Parameter > m_Requests2;   //リクエストのバッファ
    
    std::vector< Parameter >* m_Requests;   //リクエスト追加用
    std::vector< Parameter >* m_UpdatingRequests;//処理中のリクエスト
    RequestExecuter m_Executer;
    SortPredicator m_SortPred;
    
    std::mutex m_Mutex;
};

int main(int argc, const char * argv[])
{    
    RequestUpdate<int> requestUpdate;
    
    // リクエストを処理する関数を登録
    requestUpdate.SetRequestExecuter( [](int v) {
        std::cout << v << std::endl;
    } );
    // 値を小さい順に評価するようにソート関数を登録
    requestUpdate.SetRequestSortPredicator([]( std::tuple<int> v1, std::tuple<int> v2 ){
        return std::get<0>(v1) < std::get<0>(v2);
    } );
    
    // 適当にパラメータを登録
    requestUpdate.AddRequest(100);
    requestUpdate.AddRequest(200);
    requestUpdate.AddRequest(400);
    requestUpdate.AddRequest(100);
    
    // リクエストの順番に関わらずソートされた順番で処理される
    requestUpdate.Update();
    
    return 0;
}

