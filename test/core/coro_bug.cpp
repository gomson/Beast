//
// Copyright (c) 2013-2017 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#include <beast/unit_test/suite.hpp>
#include <beast/test/yield_to.hpp>
#include <beast/core/handler_helpers.hpp>
#include <boost/asio.hpp>
#include <memory>
#include <string>

namespace beast {

class coro_bug_test
    : public beast::unit_test::suite
    , public beast::test::enable_yield_to
{
public:
    using error_code = boost::system::error_code;
    using endpoint_type = boost::asio::ip::tcp::endpoint;
    using address_type = boost::asio::ip::address;
    using socket_type = boost::asio::ip::tcp::socket;

    class server
    {
        boost::asio::io_service ios_;
        boost::asio::ip::tcp::acceptor acceptor_;
        std::thread thread_;

    public:
        server()
            : acceptor_(ios_)
        {
            auto const ep = endpoint_type{
                address_type::from_string("127.0.0.1"), 0};
            acceptor_.open(ep.protocol());
            acceptor_.set_option(
                boost::asio::socket_base::reuse_address{true});
            acceptor_.bind(ep);
            acceptor_.listen(
                boost::asio::socket_base::max_connections);
            thread_ = std::thread{&server::run_one, this};
        }

        ~server()
        {
            thread_.join();
        }

        endpoint_type
        local_endpoint() const
        {
            return acceptor_.local_endpoint();
        }

        void
        run_one()
        {
            socket_type s{ios_};
            acceptor_.accept(s);
            boost::asio::streambuf b;
            boost::asio::read_until(s, b, "\n");
            boost::asio::write(s, b.data());
            s.shutdown(socket_type::shutdown_both);
            s.close();
        }
    };

    // Simple composed operation to write a string then read a string
    template<class AsyncStream, class Handler>
    class echo_op
    {
        struct data
        {
            Handler h;
            AsyncStream& s;
            boost::asio::streambuf b;
            std::string str;
            int state = 0;

            template<class DeducedHandler>
            data(AsyncStream& s_, std::string str_,
                    DeducedHandler&& h_)
                : h(std::forward<DeducedHandler>(h_))
                , s(s_)
                , str(std::move(str_))
            {
            }
        };

        std::shared_ptr<data> d_;

    public:
        echo_op(echo_op&&) = default;
        echo_op(echo_op const&) = default;

        template<class... Args>
        echo_op(AsyncStream& s, Args&&... args)
            : d_(std::make_shared<data>(
                s, std::forward<Args>(args)...))
        {
            (*this)(error_code{}, 0);
        }

        friend
        void* asio_handler_allocate(
            std::size_t size, echo_op* op)
        {
            return beast_asio_helpers::
                allocate(size, op->d_->h);
        }

        friend
        void asio_handler_deallocate(
            void* p, std::size_t size, echo_op* op)
        {
            return beast_asio_helpers::
                deallocate(p, size, op->d_->h);
        }

        friend
        bool asio_handler_is_continuation(echo_op* op)
        {
            return op->d_->state != 0 ||
                beast_asio_helpers::is_continuation(op->h);
        }

        template<class Function>
        friend
        void asio_handler_invoke(Function&& f, echo_op* op)
        {
            return beast_asio_helpers::
                invoke(f, op->d_->h);
        }

        void
        operator()(error_code ec, std::size_t bytes_transferred)
        {
            if(! ec)
            {
                switch(d_->state)
                {
                case 0:
                {
                    d_->state = 1;
                    boost::asio::async_write(d_->s,
                        boost::asio::buffer(
                            d_->str.data(), d_->str.size()),
                                std::move(*this));
                    return;
                }
                case 1:
                {
                    d_->state = 2;
                #if 0
                    boost::asio::async_read(d_->s,
                        d_->b, std::move(*this));
                #else
                    // This blows up
                    boost::asio::async_read(d_->s,
                        d_->b, std::move(d_->h));
                #endif
                    return;
                }
                case 2:
                {
                    break;
                }
                }
            }
            d_->h(ec, bytes_transferred);
        }
    };

    template<class AsyncStream, class Handler>
    BOOST_ASIO_INITFN_RESULT_TYPE(Handler,
        void (boost::system::error_code, std::size_t))
    async_echo(AsyncStream& s, std::string str, Handler&& h)
    {
        boost::asio::detail::async_result_init<Handler,
            void (boost::system::error_code, std::size_t)> init{
                std::forward<Handler>(h)};
        echo_op<AsyncStream, typename boost::asio::handler_type<
            Handler, void (boost::system::error_code, std::size_t)>::type>{
                s, std::move(str), init.handler};
        return init.result.get();
    }

    void
    run()
    {
        server ss;
        auto const ep = ss.local_endpoint();
        socket_type s{get_io_service()};
        s.connect(ep);
        yield_to(
            [&](yield_context yield)
            {
                error_code ec;
                async_echo(s, "Hello, world!\n", yield[ec]);
            });
        pass();
    }

};

BEAST_DEFINE_TESTSUITE(coro_bug,core,beast);

} // beast

