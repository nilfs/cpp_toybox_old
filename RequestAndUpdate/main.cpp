//
//  main.cpp
//  RequestAndUpdate
//
//  Created by nilfs on 13/07/06.
//  Copyright (c) 2013年 nilfs. All rights reserved.
//

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

