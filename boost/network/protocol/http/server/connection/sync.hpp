// Copyright 2009 (c) Dean Michael Berris <mikhailberis@gmail.com>
// Copyright 2009 (c) Tarroo, Inc.
// Adapted from Christopher Kholhoff's Boost.Asio Example, released under
// the Boost Software License, Version 1.0. (See acccompanying file LICENSE_1_0.txt
// or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_NETWORK_HTTP_SERVER_SYNC_CONNECTION_HPP_
#define BOOST_NETWORK_HTTP_SERVER_SYNC_CONNECTION_HPP_

#ifndef BOOST_NETWORK_HTTP_SERVER_CONNECTION_BUFFER_SIZE
#define BOOST_NETWORK_HTTP_SERVER_CONNECTION_BUFFER_SIZE 4096uL
#endif

#include <boost/enable_shared_from_this.hpp>
#include <boost/network/protocol/http/server/request_parser.hpp>
#include <boost/network/protocol/http/request.hpp>
#include <boost/network/protocol/http/response.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/placeholders.hpp>
#include <boost/array.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/bind.hpp>
#include <boost/network/protocol/http/algorithms/flatten.hpp>

namespace boost { namespace network { namespace http {

#ifndef BOOST_NETWORK_NO_LIB
  extern void parse_version(std::string const & partial_parsed, fusion::tuple<uint8_t,uint8_t> & version_pair);
  extern void parse_headers(std::string const & input, std::vector<std::pair<std::string,std::string> > & container);
#endif

class sync_server_connection : public boost::enable_shared_from_this<sync_server_connection> {
 public:
  sync_server_connection(boost::asio::io_service & service,
             function<void(request const &, response &)> handler)
  : service_(service)
  , handler_(handler)
  , socket_(service_)
  , wrapper_(service_)
  {
  }

  boost::asio::ip::tcp::socket & socket() {
    return socket_;
  }

  void start() {
    using boost::asio::ip::tcp;
    boost::system::error_code option_error;
    // TODO make no_delay an option in server_options.
    socket_.set_option(tcp::no_delay(true), option_error);
    std::ostringstream ip_stream;
    ip_stream << socket_.remote_endpoint().address().to_string() << ':'
      << socket_.remote_endpoint().port();
    request_.set_source(ip_stream.str());
    socket_.async_read_some(
      boost::asio::buffer(read_buffer_),
      wrapper_.wrap(
        boost::bind(
          &sync_server_connection::handle_read_data,
          sync_server_connection::shared_from_this(),
          method,
          boost::asio::placeholders::error,
          boost::asio::placeholders::bytes_transferred)));
  }

 private:
  
  enum state_t {
      method, uri, version, headers, body
  };


  void handle_read_data(state_t state, boost::system::error_code const & ec, std::size_t bytes_transferred) {
    if (!ec) {
      logic::tribool parsed_ok;
      iterator_range<buffer_type::iterator> result_range, input_range;
      data_end = read_buffer_.begin();
      std::advance(data_end, bytes_transferred);
      switch (state) {
        case method:
          input_range = boost::make_iterator_range(
            new_start, data_end);
          fusion::tie(parsed_ok, result_range) = parser_.parse_until(
            request_parser::method_done, input_range);
          if (!parsed_ok) { 
            client_error();
            break;
          } else if (parsed_ok == true) {
            std::string method;
            swap(partial_parsed, method);
            method.append(boost::begin(result_range),
                    boost::end(result_range));
            trim(method);
            request_.set_method(method);
            new_start = boost::end(result_range);
            // Determine whether we're going to need to parse the body of the
            // request. All we do is peek at the first character of the method
            // to determine whether it's a POST or a PUT.
            read_body_ = method.size() ? method[0] == 'P' : false;
          } else {
            partial_parsed.append(
              boost::begin(result_range),
              boost::end(result_range));
            new_start = read_buffer_.begin();
            read_more(method);
            break;
          }
        case uri:
          input_range = boost::make_iterator_range(
            new_start, data_end);
          fusion::tie(parsed_ok, result_range) = parser_.parse_until(
            request_parser::uri_done,
            input_range);
          if (!parsed_ok) {
            client_error();
            break;
          } else if (parsed_ok == true) {
            std::string destination;
            swap(partial_parsed, destination);
            destination.append(boost::begin(result_range),
                       boost::end(result_range));
            trim(destination);
            request_.set_destination(destination);
            new_start = boost::end(result_range);
          } else {
            partial_parsed.append(
              boost::begin(result_range),
              boost::end(result_range));
            new_start = read_buffer_.begin();
            read_more(uri);
            break;
          }
        case version:
          input_range = boost::make_iterator_range(
            new_start, data_end);
          fusion::tie(parsed_ok, result_range) = parser_.parse_until(
            request_parser::version_done,
            input_range);
          if (!parsed_ok) {
            client_error();
            break;
          } else if (parsed_ok == true) {
            fusion::tuple<uint8_t, uint8_t> version_pair;
            partial_parsed.append(boost::begin(result_range), boost::end(result_range));
            parse_version(partial_parsed, version_pair);
            request_.set_version_major(fusion::get<0>(version_pair));
            request_.set_version_minor(fusion::get<1>(version_pair));
            new_start = boost::end(result_range);
            partial_parsed.clear();
          } else {
            partial_parsed.append(
              boost::begin(result_range),
              boost::end(result_range));
            new_start = read_buffer_.begin();
            read_more(version);
            break;
          }
        case headers:
          input_range = boost::make_iterator_range(
            new_start, data_end);
          fusion::tie(parsed_ok, result_range) = parser_.parse_until(
            request_parser::headers_done,
            input_range);
          if (!parsed_ok) {
            client_error();
            break;
          } else if (parsed_ok == true) {
            partial_parsed.append(
              boost::begin(result_range),
              boost::end(result_range));
            std::vector<std::pair<std::string, std::string> > headers;
            parse_headers(partial_parsed, headers);
            for (std::vector<std::pair<std::string, std::string> >::const_iterator it = headers.begin();
               it != headers.end();
               ++it) {
            request_.append_header(it->first, it->second);
            }
            new_start = boost::end(result_range);
            if (read_body_) {
            } else {
              response response_;
              handler_(request_, response_);
              std::vector<asio::const_buffer> response_buffers;
              flatten(response_, response_buffers);
              boost::asio::async_write(
                socket_,
                response_buffers,
                wrapper_.wrap(
                  boost::bind(
                    &sync_server_connection::handle_write,
                    sync_server_connection::shared_from_this(),
                    boost::asio::placeholders::error)));
            }
            return;
          } else {
            partial_parsed.append(
              boost::begin(result_range),
              boost::end(result_range));
            new_start = read_buffer_.begin();
            read_more(headers);
            break;
          }
        default:
          BOOST_ASSERT(false && "This is a bug, report to the cpp-netlib devel mailing list!");
          std::abort();
      }
    } else {
      error_encountered = in_place<boost::system::system_error>(ec);
    }
  }

  void handle_write(system::error_code const &ec) {
    if (ec) {
      // TODO maybe log the error here.
    }
  }

  void client_error() {
      static char const * bad_request = 
          "HTTP/1.0 400 Bad Request\r\nConnection: close\r\nContent-Type: text/plain\r\nContent-Length: 12\r\n\r\nBad Request.";

      asio::async_write(
          socket()
          , asio::buffer(bad_request, strlen(bad_request))
          , wrapper_.wrap(
              boost::bind(
                  &sync_server_connection::client_error_sent
                  , sync_server_connection::shared_from_this()
                  , asio::placeholders::error
                  , asio::placeholders::bytes_transferred)));
  }

  void client_error_sent(boost::system::error_code const & ec, std::size_t bytes_transferred) {
      if (!ec) {
          boost::system::error_code ignored;
          socket().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
          socket().close(ignored);
      } else {
          error_encountered = in_place<boost::system::system_error>(ec);
      }
  }

  void read_more(state_t state) {
      socket_.async_read_some(
          asio::buffer(read_buffer_)
          , wrapper_.wrap(
              boost::bind(
                  &sync_server_connection::handle_read_data,
                  sync_server_connection::shared_from_this(),
                  state,
                  boost::asio::placeholders::error,
                  boost::asio::placeholders::bytes_transferred
                  )
              )
          );
  }

  boost::asio::io_service & service_;
  function<void(request const &, response &)> handler_;
  boost::asio::ip::tcp::socket socket_;
  boost::asio::io_service::strand wrapper_;

  typedef boost::array<char,BOOST_NETWORK_HTTP_SERVER_CONNECTION_BUFFER_SIZE> buffer_type;
  buffer_type read_buffer_;
  buffer_type::iterator new_start, data_end;
  request_parser parser_;
  request request_;
  response response_;
  std::string partial_parsed;
  optional<system::system_error> error_encountered;
  bool read_body_;
};


} // namespace http

} // namespace network

} // namespace boost

#endif // BOOST_NETWORK_HTTP_SERVER_SYNC_CONNECTION_HPP_

