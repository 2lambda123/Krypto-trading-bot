#ifndef K_DATA_H_
#define K_DATA_H_
//! \file
//! \brief Data transfer/transform helpers.

namespace ₿ {
  class WebSocketFrames {
    protected:
      static const string frame(string data, const int &opcode, const bool &mask) {
        const int key = mask ? rand() : 0;
        const int bit = mask ? 0x80   : 0;
        size_t pos = 0,
               len = data.length();
                                data.insert(pos++  , 1,  opcode     | 0x80);
        if      (len <= 0x7D)   data.insert(pos++  , 1,  len        | bit );
        else if (len <= 0xFFFF) data.insert(pos    , 1, (char)(0x7E | bit))
                                    .insert(pos + 1, 1, (len >>  8) & 0xFF)
                                    .insert(pos + 2, 1,  len        & 0xFF), pos += 3;
        else                    data.insert(pos    , 1, (char)(0x7F | bit))
                                    .insert(pos + 1, 4,                  0)
                                    .insert(pos + 5, 1, (len >> 24) & 0xFF)
                                    .insert(pos + 6, 1, (len >> 16) & 0xFF)
                                    .insert(pos + 7, 1, (len >>  8) & 0xFF)
                                    .insert(pos + 8, 1,  len        & 0xFF), pos += 9;
        if (mask) {             data.insert(pos    , 1, (key >> 24) & 0xFF)
                                    .insert(pos + 1, 1, (key >> 16) & 0xFF)
                                    .insert(pos + 2, 1, (key >>  8) & 0xFF)
                                    .insert(pos + 3, 1,  key        & 0xFF), pos += 4;
          for (size_t i = 0; i < len; i++)
            data.at(pos + i) ^= data.at(pos - 4 + (i % 4));
        }
        return data;
      };
      static const string unframe(string &data, const function<void(const string&)> &pong, const function<void()> &drop) {
        string msg;
        const size_t max = data.length();
        if (max > 1) {
          const bool flat = (data[0] & 0x40) != 0x40,
                     mask = (data[1] & 0x80) == 0x80;
          const size_t key = mask ? 4 : 0;
          size_t                            len =    data[1] & 0x7F,          pos = key;
          if      (            len <= 0x7D)                                   pos += 2;
          else if (max > 2 and len == 0x7E) len = (((data[2] & 0xFF) <<  8)
                                                |   (data[3] & 0xFF)       ), pos += 4;
          else if (max > 8 and len == 0x7F) len = (((data[6] & 0xFF) << 24)
                                                |  ((data[7] & 0xFF) << 16)
                                                |  ((data[8] & 0xFF) <<  8)
                                                |   (data[9] & 0xFF)       ), pos += 10;
          if (!flat or pos == key) drop();
          else if (max >= pos + len) {
            if (mask)
              for (size_t i = 0; i < len; i++)
                data.at(pos + i) ^= data.at(pos - key + (i % key));
            const unsigned char opcode = data[0] & 0x0F;
            if (opcode == 0x09)
              pong(frame(data.substr(pos, len), 0x0A, !mask));
            else if (opcode == 0x02 or opcode == 0x0A or opcode == 0x08
              or ((data[0] & 0x80) != 0x80 and (opcode == 0x00 or opcode == 0x01))
            ) {
              if (opcode == 0x08) drop();
            } else
              msg = data.substr(pos, len);
            data = data.substr(pos + len);
          }
        }
        return msg;
      };
  };

  class FixFrames {
    protected:
      static const string frame(string data, const string &type, const unsigned long &sequence, const string &apikey, const string &target) {
        data = "35=" + type                     + "\u0001"
               "49=" + apikey                   + "\u0001"
               "56=" + target                   + "\u0001"
               "34=" + to_string(sequence)      + "\u0001"
             + data;
        data = "8=FIX.4.2"                        "\u0001"
               "9="  + to_string(data.length()) + "\u0001"
             + data;
        char ch = 0;
        for (size_t i = data.length(); i --> 0; ch += data.at(i));
        stringstream sum;
        sum << setfill('0')
            << setw(3)
            << (ch & 0xFF);
        data += "10=" + sum.str()               + "\u0001";
        return data;
      };
      static const string unframe(string &data, const function<void(const string&)> &pong, const function<void()> &drop) {
        string msg;
        const size_t end = data.find("\u0001" "10=");
        if (end != string::npos and data.length() > end + 7) {
          string raw = data.substr(0, end + 8);
          data = data.substr(raw.length());
          if (raw.find("\u0001" "35=0" "\u0001") != string::npos
            or raw.find("\u0001" "35=1" "\u0001") != string::npos
          ) pong("0");
          else if (raw.find("\u0001" "35=5" "\u0001") != string::npos) {
            pong("5");
            drop();
          } else {
            size_t tok;
            while ((tok = raw.find("\u0001")) != string::npos) {
              raw.replace(raw.find("="), 1, "\":\"");
              msg += "\"" + raw.substr(0, tok + 2) + "\",";
              raw = raw.substr(tok + 3);
            }
            msg.pop_back();
            msg = "{" + msg + "}";
          }
        }
        return msg;
      };
  };

  class Curl {
    public:
      static function<void(CURL*)> global_setopt;
    private:
      class Easy {
        public:
          static void cleanup(CURL *&curl, curl_socket_t &sockfd) {
            if (curl) curl_easy_cleanup(curl);
            curl   = nullptr;
            sockfd = 0;
          };
          static const CURLcode receive(CURL *&curl, curl_socket_t &sockfd, string &buffer) {
            CURLcode rc = CURLE_COULDNT_CONNECT;
            if (curl and sockfd and CURLE_OPERATION_TIMEDOUT == (rc = recv(curl, sockfd, buffer, 0)))
              rc = CURLE_OK;
            if (rc != CURLE_OK)
              cleanup(curl, sockfd);
            return rc;
          };
        protected:
          static const CURLcode connect(CURL *&curl, curl_socket_t &sockfd, string &buffer, const string &url, const string &header, const string &res1, const string &res2) {
            buffer.clear();
            CURLcode rc;
            if (CURLE_OK == (rc = init(curl, sockfd))) {
              global_setopt(curl);
              curl_easy_setopt(curl, CURLOPT_URL, url.data());
              curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);
              if ( CURLE_OK != (rc = curl_easy_perform(curl))
                or CURLE_OK != (rc = curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sockfd))
                or CURLE_OK != (rc = send(curl, sockfd, header))
                or CURLE_OK != (rc = recv(curl, sockfd, buffer, 5))
                or string::npos == buffer.find(res1)
                or string::npos == buffer.find(res2)
              ) {
                if (rc == CURLE_OK)
                  rc = CURLE_WEIRD_SERVER_REPLY;
                cleanup(curl, sockfd);
              }
            }
            return rc;
          };
          static const CURLcode emit(CURL *&curl, curl_socket_t &sockfd, const string &data) {
            CURLcode rc = CURLE_COULDNT_CONNECT;
            if (!curl or !sockfd or CURLE_OK != (rc = send(curl, sockfd, data)))
              cleanup(curl, sockfd);
            return rc;
          };
        private:
          static const CURLcode init(CURL *&curl, curl_socket_t &sockfd) {
            if (!curl) curl = curl_easy_init();
            else curl_easy_reset(curl);
            sockfd = 0;
            return curl
              ? CURLE_OK
              : CURLE_FAILED_INIT;
          };
          static const CURLcode send(CURL *curl, const curl_socket_t &sockfd, const string &data) {
            CURLcode rc;
            size_t len  = data.length(),
                   sent = 0;
            do {
              do {
                size_t n = 0;
                rc = curl_easy_send(curl, data.substr(sent).data(), len - sent, &n);
                sent += n;
                if (rc == CURLE_AGAIN and !wait(sockfd, false, 5))
                  return CURLE_OPERATION_TIMEDOUT;
              } while (rc == CURLE_AGAIN);
              if (rc != CURLE_OK) break;
            } while (sent < len);
            return rc;
          };
          static const CURLcode recv(CURL *curl, curl_socket_t &sockfd, string &buffer, const int &timeout) {
            CURLcode rc;
            for(;;) {
              char data[524288];
              size_t n;
              do {
                n = 0;
                rc = curl_easy_recv(curl, data, sizeof(data), &n);
                buffer.append(data, n);
                if (rc == CURLE_AGAIN and !wait(sockfd, true, timeout))
                  return CURLE_OPERATION_TIMEDOUT;
              } while (rc == CURLE_AGAIN);
              if ((timeout and buffer.find("\r\n\r\n") != buffer.find("\u0001" "10="))
                or rc != CURLE_OK
                or n == 0
              ) break;
            }
            return rc;
          };
          static const int wait(const curl_socket_t &sockfd, const bool &io, const int &timeout) {
            struct timeval tv = {timeout, 10000};
            fd_set infd,
                   outfd;
            FD_ZERO(&infd);
            FD_ZERO(&outfd);
            FD_SET(sockfd, io ? &infd : &outfd);
            return select(sockfd + 1, &infd, &outfd, nullptr, &tv);
          };
      };
    public_friend:
      class Web {
        public:
          static const json xfer(const string &url, const long &timeout = 13) {
            return request(url, [&](CURL *curl) {
              curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
            });
          };
          static const json xfer(const string &url, const string &post) {
            return request(url, [&](CURL *curl) {
              struct curl_slist *h_ = nullptr;
              h_ = curl_slist_append(h_, "Content-Type: application/x-www-form-urlencoded");
              curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h_);
              curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post.data());
            });
          };
          static const json request(const string &url, const function<void(CURL*)> custom_setopt) {
            static mutex waiting_reply;
            lock_guard<mutex> lock(waiting_reply);
            string reply;
            CURLcode rc = CURLE_FAILED_INIT;
            CURL *curl = curl_easy_init();
            if (curl) {
              custom_setopt(curl);
              global_setopt(curl);
              curl_easy_setopt(curl, CURLOPT_URL, url.data());
              curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write);
              curl_easy_setopt(curl, CURLOPT_WRITEDATA, &reply);
              rc = curl_easy_perform(curl);
              curl_easy_cleanup(curl);
            }
            return rc == CURLE_OK
              ? (json::accept(reply)
                  ? json::parse(reply)
                  : json::object()
                )
              : (json){ {"error", string("CURL Request Error: ") + curl_easy_strerror(rc)} };
          };
        private:
          static size_t write(void *buf, size_t size, size_t nmemb, void *reply) {
            ((string*)reply)->append((char*)buf, size *= nmemb);
            return size;
          };
      };
      class WebSocket: public Easy,
                       public WebSocketFrames {
        public:
          static const CURLcode connect(CURL *&curl, curl_socket_t &sockfd, string &buffer, const string &uri) {
            CURLcode rc = CURLE_URL_MALFORMAT;
            CURLU *url = curl_url();
            char *host,
                 *port,
                 *path;
            if (  !curl_url_set(url, CURLUPART_URL, ("http" + uri.substr(2)).data(), 0)
              and !curl_url_get(url, CURLUPART_HOST, &host, 0)
              and !curl_url_get(url, CURLUPART_PORT, &port, CURLU_DEFAULT_PORT)
              and !curl_url_get(url, CURLUPART_PATH, &path, 0)
              and CURLE_OK == (rc = Easy::connect(curl, sockfd, buffer,
                "http" + uri.substr(2),
                "GET " + string(path) + " HTTP/1.1"
                  "\r\n" "Host: " + string(host) + ":" + string(port) +
                  "\r\n" "Upgrade: websocket"
                  "\r\n" "Connection: Upgrade"
                  "\r\n" "Sec-WebSocket-Key: x3JJHMbDL1EzLkh9GBhXDw=="
                  "\r\n" "Sec-WebSocket-Version: 13"
                  "\r\n"
                  "\r\n",
                "HTTP/1.1 101 Switching Protocols",
                "HSmrc0sMlYUkAGmm5OPpG2HaGWk="
            ))) buffer = buffer.substr(buffer.rfind("\r\n\r\n") + 4);
            curl_url_cleanup(url);
            return rc;
          };
          static const CURLcode emit(CURL *&curl, curl_socket_t &sockfd, const string &data, const int &opcode) {
            return Easy::emit(curl, sockfd, frame(data, opcode, true));
          };
          static const string unframe(CURL *&curl, curl_socket_t &sockfd, string &data) {
            return WebSocketFrames::unframe(
              data,
              [&](const string &pong) {
                Easy::emit(curl, sockfd, pong);
              },
              [&]() {
                cleanup(curl, sockfd);
              }
            );
          };
      };
      class FixSocket: public Easy,
                       public FixFrames {
        public:
          static const CURLcode connect(CURL *&curl, curl_socket_t &sockfd, string &buffer, const string &uri, string data, unsigned long &sequence, const string &apikey, const string &target) {
            CURLcode rc;
            if (CURLE_OK == (rc = Easy::connect(curl, sockfd, buffer,
              "https://" + uri,
              frame(data, "A", sequence = 1, apikey, target),
              "8=FIX.4.2" "\u0001",
              "\u0001" "35=A" "\u0001"
            ))) buffer = buffer.substr(buffer.rfind("\u0001" "10=") + 8);
            return rc;
          };
          static const CURLcode emit(CURL *&curl, curl_socket_t &sockfd, const string &data, const string &type, unsigned long &sequence, const string &apikey, const string &target) {
            return Easy::emit(curl, sockfd, frame(data, type, ++sequence, apikey, target));
          };
          static const string unframe(CURL *&curl, curl_socket_t &sockfd, string &data, unsigned long &sequence, const string &apikey, const string &target) {
            return FixFrames::unframe(
              data,
              [&](const string &pong) {
                emit(curl, sockfd, "", pong, sequence, apikey, target);
              },
              [&]() {
                cleanup(curl, sockfd);
              }
            );
          };
      };
  };

  class WebServer: public WebSocketFrames {
    public_friend:
      struct Frontend {
        curl_socket_t  sockfd;
        SSL           *ssl;
        Clock          time;
        string         addr,
                       out,
                       in;
      };
    protected:
      static const string unframe(curl_socket_t &sockfd, SSL *&ssl, string &data, string &out) {
        return WebSocketFrames::unframe(
          data,
          [&](const string &pong) {
            out += pong;
          },
          [&]() {
            shutdown(sockfd, ssl);
          }
        );
      };
      static curl_socket_t listen(const string &inet, const int &port, const bool &ipv6) {
        curl_socket_t sockfd = 0;
        struct addrinfo  hints,
                        *result,
                        *rp;
        memset(&hints, 0, sizeof(addrinfo));
        hints.ai_flags    = AI_PASSIVE;
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (!getaddrinfo(inet.empty() ? nullptr : inet.data(), to_string(port).data(), &hints, &result)) {
          if (ipv6)
            for (rp = result; rp and !sockfd; rp = sockfd ? rp : rp->ai_next)
              if (rp->ai_family == AF_INET6)
                sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
          if (!sockfd)
            for (rp = result; rp and !sockfd; rp = sockfd ? rp : rp->ai_next)
              if (rp->ai_family == AF_INET)
                sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
          if (sockfd) {
            int enabled = 1;
            setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(int));
            if (::bind(sockfd, rp->ai_addr, rp->ai_addrlen) || ::listen(sockfd, 512))
              shutdown(sockfd);
          }
          freeaddrinfo(result);
        }
        return sockfd;
      };
      static const bool accept_requests(const curl_socket_t &sockfd, SSL_CTX *&ctx, vector<Frontend> &requests) {
        curl_socket_t clientfd;
#if defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK)
        clientfd = accept4(sockfd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
#else
        clientfd = accept(sockfd, nullptr, nullptr);
#endif

#ifdef __APPLE__
        if (clientfd != -1) {
            int noSigpipe = 1;
            setsockopt(clientfd, SOL_SOCKET, SO_NOSIGPIPE, &noSigpipe, sizeof(int));
        }
#endif
        if (clientfd != -1) {
          SSL *ssl = nullptr;
          if (ctx) {
            ssl = SSL_new(ctx);
            SSL_set_accept_state(ssl);
            SSL_set_fd(ssl, clientfd);
            SSL_set_mode(ssl, SSL_MODE_RELEASE_BUFFERS);
          }
          requests.push_back({clientfd, ssl, Tstamp});
        }
        return clientfd > 0;
      };
      static void io(curl_socket_t &sockfd, SSL *&ssl, string &in, string &out, const bool &persist) {
        if (ssl) {
          if (!out.empty()) {
            cork(sockfd, 1);
            int n = SSL_write(ssl, out.data(), (int)out.length());
            switch (SSL_get_error(ssl, n)) {
              case SSL_ERROR_NONE:        out = out.substr(n);
              case SSL_ERROR_ZERO_RETURN: if (!persist) {
                                            shutdown(sockfd, ssl);
                                            return;
                                          }
              case SSL_ERROR_WANT_READ:
              case SSL_ERROR_WANT_WRITE:  break;
              default:                    shutdown(sockfd, ssl);
                                          return;
            }
            cork(sockfd, 0);
          }
          do {
            char data[1024];
            int n = SSL_read(ssl, data, sizeof(data));
            switch (SSL_get_error(ssl, n)) {
              case SSL_ERROR_NONE:        in.append(data, n);
              case SSL_ERROR_ZERO_RETURN:
              case SSL_ERROR_WANT_READ:
              case SSL_ERROR_WANT_WRITE:  break;
              default:                    if (!persist)
                                            shutdown(sockfd, ssl);
                                          return;
            }
          } while (SSL_pending(ssl));
        } else {
          if (!out.empty()) {
            cork(sockfd, 1);
            ssize_t n = ::send(sockfd, out.data(), out.length(), MSG_NOSIGNAL);
            if (n > 0) out = out.substr(n);
            if (persist) {
              if (n < 0)  {
                shutdown(sockfd);
                return;
              }
            } else if (out.empty()) {
              shutdown(sockfd);
              return;
            }
            cork(sockfd, 0);
          }
          char data[1024];
          ssize_t n = ::recv(sockfd, data, sizeof(data), 0);
          if (n > 0) in.append(data, n);
        }
      };
      static const string address(const curl_socket_t &sockfd) {
        string addr;
#ifndef _WIN32
        sockaddr_storage ss;
        socklen_t len = sizeof(ss);
        if (getpeername(sockfd, (sockaddr*)&ss, &len) != -1) {
          char buf[INET6_ADDRSTRLEN];
          if (ss.ss_family == AF_INET) {
            auto *ipv4 = (sockaddr_in*)&ss;
            inet_ntop(AF_INET, &ipv4->sin_addr, buf, sizeof(buf));
          } else {
            auto *ipv6 = (sockaddr_in6*)&ss;                                    //-V641
            inet_ntop(AF_INET6, &ipv6->sin6_addr, buf, sizeof(buf));
          }
          addr = string(buf);
          if (addr.length() > 7 and addr.substr(0, 7) == "::ffff:") addr = addr.substr(7);
          if (addr.length() < 7) addr.clear();
        }
#endif
        return addr.empty() ? "unknown" : addr;
      };
      static void shutdown(curl_socket_t &sockfd, SSL *&ssl) {
        if (ssl) {
          SSL_shutdown(ssl);
          SSL_free(ssl);
        }
        shutdown(sockfd);
      };
      static void shutdown(curl_socket_t &sockfd) {
#ifdef _WIN32
        closesocket(sockfd);
#else
        close(sockfd);
#endif
        sockfd = 0;
      };
      static const vector<string> ssl_context(const string &crt, const string &key, SSL_CTX *&ctx) {
        vector<string> warn;
        ctx = SSL_CTX_new(SSLv23_server_method());
        if (ctx) {
          SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv3);
          if (crt.empty() or key.empty()) {
            if (!crt.empty())
              warn.emplace_back("Ignored .crt file because .key file is missing");
            if (!key.empty())
              warn.emplace_back("Ignored .key file because .crt file is missing");
            warn.emplace_back("Connected web clients will enjoy unsecure SSL encryption..\n"
              "(because the private key is visible in the source!). See --help argument to setup your own SSL");
            if (!SSL_CTX_use_certificate(ctx,
              PEM_read_bio_X509(BIO_new_mem_buf((void*)
                "-----BEGIN CERTIFICATE-----"                                      "\n"
                "MIICATCCAWoCCQCiyDyPL5ov3zANBgkqhkiG9w0BAQsFADBFMQswCQYDVQQGEwJB" "\n"
                "VTETMBEGA1UECAwKU29tZS1TdGF0ZTEhMB8GA1UECgwYSW50ZXJuZXQgV2lkZ2l0" "\n"
                "cyBQdHkgTHRkMB4XDTE2MTIyMjIxMDMyNVoXDTE3MTIyMjIxMDMyNVowRTELMAkG" "\n"
                "A1UEBhMCQVUxEzARBgNVBAgMClNvbWUtU3RhdGUxITAfBgNVBAoMGEludGVybmV0" "\n"
                "IFdpZGdpdHMgUHR5IEx0ZDCBnzANBgkqhkiG9w0BAQEFAAOBjQAwgYkCgYEAunyx" "\n"
                "1lNsHkMmCa24Ns9xgJAwV3A6/Jg/S5jPCETmjPRMXqAp89fShZxN2b/2FVtU7q/N" "\n"
                "EtNpPyEhfAhPwYrkHCtip/RmZ/b6qY2Cx6otFIsuwO8aUV27CetpoM8TAQSuufcS" "\n"
                "jcZD9pCAa9GM/yWeqc45su9qBBmLnAKYuYUeDQUCAwEAATANBgkqhkiG9w0BAQsF" "\n"
                "AAOBgQAeZo4zCfnq5/6gFzoNDKg8DayoMnCtbxM6RkJ8b/MIZT5p6P7OcKNJmi1o" "\n"
                "XD2evdxNrY0ObQ32dpiLqSS1JWL8bPqloGJBNkSPi3I+eBoJSE7/7HOroLNbp6nS" "\n"
                "aaec6n+OlGhhjxn0DzYiYsVBUsokKSEJmHzoLHo3ZestTTqUwg=="             "\n"
                "-----END CERTIFICATE-----"                                        "\n"
              , -1), nullptr, nullptr, nullptr
            )) or !SSL_CTX_use_RSAPrivateKey(ctx,
              PEM_read_bio_RSAPrivateKey(BIO_new_mem_buf((void*)
                "-----BEGIN RSA PRIVATE KEY-----"                                  "\n"
                "MIICXAIBAAKBgQC6fLHWU2weQyYJrbg2z3GAkDBXcDr8mD9LmM8IROaM9ExeoCnz" "\n"
                "19KFnE3Zv/YVW1Tur80S02k/ISF8CE/BiuQcK2Kn9GZn9vqpjYLHqi0Uiy7A7xpR" "\n"
                "XbsJ62mgzxMBBK659xKNxkP2kIBr0Yz/JZ6pzjmy72oEGYucApi5hR4NBQIDAQAB" "\n"
                "AoGBAJi9OrbtOreKjeQNebzCqRcAgeeLz3RFiknzjVYbgK1gBhDWo6XJVe8C9yxq" "\n"
                "sjYJyQV5zcAmkaQYEaHR+OjvRiZ4UmXbItukOD+dnq7xs69n3w54FfANjkurdL2M" "\n"
                "fPAQm/GJT4TSBDIr7eJQPOrork9uxQStwADTqvklVlKm2YldAkEA80ZYaLrGOBbz" "\n"
                "5871ewKxtVJNCCmXdYUwq7nI/lqsLBZnB+wiwnQ+3tgfi4YoUoTnv0hIIwkyLYl9" "\n"
                "Z2wqensf6wJBAMQ96gUGnIcYJzknB5CYDNQalcvvTx7tLtgRXDf47bQJ3X/Q5k/t" "\n"
                "yDlByUBqvYVShXWs+d4ynNKLze/w18H8Os8CQBYFDAOOxFpXWYRl6zpTKBqtdGOE" "\n"
                "wDzW7WzdyB+dvW/QJ0tESHEpbHdnQJO0dPnjJcbemAjz0CLnCv7Nf5rOgjkCQE3Q" "\n"
                "izIw+/JptmvoOQyx7ixQ2mNCYmpN/Iw63gln0MHaQ5WCPUEmdYWWu3mqmbn7Deaq" "\n"
                "j233Pc4TF7b0FmnaXWsCQAVvyLVU3a9Yactb5MXaN+rEYjUW37GSo+Q1lXfm0OwF" "\n"
                "EJB7X66Bavwg4MCfpGykS71OxhTEfDu+y1gylPMCGHY="                     "\n"
                "-----END RSA PRIVATE KEY-----"                                    "\n"
              , -1), nullptr, nullptr, nullptr)
            )) ctx = nullptr;
          } else {
            if (access(crt.data(), R_OK) == -1)
              warn.emplace_back("Unable to read SSL .crt file at " + crt);
            if (access(key.data(), R_OK) == -1)
              warn.emplace_back("Unable to read SSL .key file at " + key);
            if (!SSL_CTX_use_certificate_chain_file(ctx, crt.data())
              or !SSL_CTX_use_RSAPrivateKey_file(ctx, key.data(), SSL_FILETYPE_PEM)
            ) {
              ctx = nullptr;
              warn.emplace_back("Unable to encrypt web clients, will fallback to plain text");
            }
          }
        }
        return warn;
      };
      const string document(const string &content, const unsigned int &code, const string &type) {
        string headers;
        if      (code == 200) headers = "HTTP/1.1 200 OK"
                                        "\r\n" "Connection: keep-alive"
                                        "\r\n" "Accept-Ranges: bytes"
                                        "\r\n" "Vary: Accept-Encoding"
                                        "\r\n" "Cache-Control: public, max-age=0";
        else if (code == 401) headers = "HTTP/1.1 401 Unauthorized"
                                        "\r\n" "Connection: keep-alive"
                                        "\r\n" "Accept-Ranges: bytes"
                                        "\r\n" "Vary: Accept-Encoding"
                                        "\r\n" "WWW-Authenticate: Basic realm=\"Basic Authorization\"";
        else if (code == 403) headers = "HTTP/1.1 403 Forbidden"
                                        "\r\n" "Connection: keep-alive"
                                        "\r\n" "Accept-Ranges: bytes"
                                        "\r\n" "Vary: Accept-Encoding";
        else if (code == 418) headers = "HTTP/1.1 418 I'm a teapot";
        else                  headers = "HTTP/1.1 404 Not Found";
        return headers
             + string((content.length() > 2 and (content.substr(0, 2) == "PK" or (
                 content.at(0) == '\x1F' and content.at(1) == '\x8B'
             ))) ? "\r\n" "Content-Encoding: gzip" : "")
             + "\r\n" "Content-Type: "   + type
             + "\r\n" "Content-Length: " + to_string(content.length())
             + "\r\n"
               "\r\n"
             + content;
      };
    private:
      static curl_socket_t socket(const int &domain, const int &type, const int &protocol) {
        const int flags =
#if defined(SOCK_CLOEXEC) and defined(SOCK_NONBLOCK)
        SOCK_CLOEXEC | SOCK_NONBLOCK;
#else
        0
#endif
        ;
        curl_socket_t sockfd = ::socket(domain, type | flags, protocol);
#ifdef __APPLE__
        if (sockfd != -1) {
          int noSigpipe = 1;
          setsockopt(sockfd, SOL_SOCKET, SO_NOSIGPIPE, &noSigpipe, sizeof(int));
        }
#endif
        return sockfd == -1
             ? 0
             : sockfd;
      };
      static void cork(const curl_socket_t &sockfd, const int &enable) {
#if defined(TCP_CORK)
        setsockopt(sockfd, IPPROTO_TCP, TCP_CORK, &enable, sizeof(int));
#elif defined(TCP_NOPUSH)
        setsockopt(sockfd, IPPROTO_TCP, TCP_NOPUSH, &enable, sizeof(int));
        if (!enable) ::send(sockfd, "", 0, MSG_NOSIGNAL);
#endif
      };
  };

  class Text {
    public:
      static string strL(string input) {
        transform(input.begin(), input.end(), input.begin(), ::tolower);
        return input;
      };
      static string strU(string input) {
        transform(input.begin(), input.end(), input.begin(), ::toupper);
        return input;
      };
      static string B64(const string &input) {
        BIO *bio, *b64;
        BUF_MEM *bufferPtr;
        b64 = BIO_new(BIO_f_base64());
        bio = BIO_new(BIO_s_mem());
        bio = BIO_push(b64, bio);
        BIO_set_close(bio, BIO_CLOSE);
        BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
        BIO_write(bio, input.data(), input.length());
        BIO_flush(bio);
        BIO_get_mem_ptr(bio, &bufferPtr);
        const string output(bufferPtr->data, bufferPtr->length);
        BIO_free_all(bio);
        return output;
      };
      static string B64_decode(const string &input) {
        BIO *bio, *b64;
        char output[input.length()];
        b64 = BIO_new(BIO_f_base64());
        bio = BIO_new_mem_buf(input.data(), input.length());
        bio = BIO_push(b64, bio);
        BIO_set_close(bio, BIO_CLOSE);
        BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
        int len = BIO_read(bio, output, input.length());
        BIO_free_all(bio);
        return string(output, len);
      };
      static string SHA1(const string &input, const bool &hex = false) {
        return SHA(input, hex, ::SHA1, SHA_DIGEST_LENGTH);
      };
      static string SHA256(const string &input, const bool &hex = false) {
        return SHA(input, hex, ::SHA256, SHA256_DIGEST_LENGTH);
      };
      static string SHA512(const string &input) {
        return SHA(input, false, ::SHA512, SHA512_DIGEST_LENGTH);
      };
      static string HMAC1(const string &key, const string &input, const bool &hex = false) {
        return HMAC(key, input, hex, EVP_sha1, SHA_DIGEST_LENGTH);
      };
      static string HMAC256(const string &key, const string &input, const bool &hex = false) {
        return HMAC(key, input, hex, EVP_sha256, SHA256_DIGEST_LENGTH);
      };
      static string HMAC512(const string &key, const string &input, const bool &hex = false) {
        return HMAC(key, input, hex, EVP_sha512, SHA512_DIGEST_LENGTH);
      };
      static string HMAC384(const string &key, const string &input, const bool &hex = false) {
        return HMAC(key, input, hex, EVP_sha384, SHA384_DIGEST_LENGTH);
      };
    private:
      static string SHA(
        const string  &input,
        const bool    &hex,
        unsigned char *(*md)(const unsigned char*, size_t, unsigned char*),
        const int     &digest_len
      ) {
        unsigned char digest[digest_len];
        md((unsigned char*)input.data(), input.length(), (unsigned char*)&digest);
        char output[digest_len * 2 + 1];
        for (unsigned int i = 0; i < digest_len; i++)
          sprintf(&output[i * 2], "%02x", (unsigned int)digest[i]);
        return hex ? HEX(output) : output;
      };
      static string HMAC(
        const string &key,
        const string &input,
        const bool   &hex,
        const EVP_MD *(evp_md)(),
        const int    &digest_len
      ) {
        unsigned char* digest;
        digest = ::HMAC(
          evp_md(),
          input.data(), input.length(),
          (unsigned char*)key.data(), key.length(),
          nullptr, nullptr
        );
        char output[digest_len * 2 + 1];
        for (unsigned int i = 0; i < digest_len; i++)
          sprintf(&output[i * 2], "%02x", (unsigned int)digest[i]);
        return hex ? HEX(output) : output;
      };
      static string HEX(const string &input) {
        const unsigned int len = input.length();
        string output;
        for (unsigned int i = 0; i < len; i += 2)
          output.push_back(
            (char)(int)strtol(input.substr(i, 2).data(), nullptr, 16)
          );
        return output;
      };
  };

  class Random {
    public:
      static const unsigned long long int64() {
        static random_device rd;
        static mt19937_64 gen(rd());
        return uniform_int_distribution<unsigned long long>()(gen);
      };
      static const RandId int45Id() {
        return to_string(int64()).substr(0, 10);
      };
      static const RandId int32Id() {
        return to_string(int64()).substr(0,  8);
      };
      static const RandId char16Id() {
        string id = string(16, ' ');
        for (auto &it : id) {
         const int offset = int64() % (26 + 26 + 10);
         if      (offset < 26)      it = 'a' + offset;
         else if (offset < 26 + 26) it = 'A' + offset - 26;
         else                       it = '0' + offset - 26 - 26;
        }
        return id;
      };
      static const RandId uuid36Id() {
        string uuid = string(36, ' ');
        uuid[8]  =
        uuid[13] =
        uuid[18] =
        uuid[23] = '-';
        uuid[14] = '4';
        unsigned long long rnd = int64();
        for (auto &it : uuid)
          if (it == ' ') {
            if (rnd <= 0x02) rnd = 0x2000000 + (int64() * 0x1000000) | 0;
            rnd >>= 4;
            const int offset = (uuid[17] != ' ' and uuid[19] == ' ')
              ? ((rnd & 0xf) & 0x3) | 0x8
              : rnd & 0xf;
            if (offset < 10) it = '0' + offset;
            else             it = 'a' + offset - 10;
          }
        return uuid;
      };
      static const RandId uuid32Id() {
        RandId uuid = uuid36Id();
        uuid.erase(remove(uuid.begin(), uuid.end(), '-'), uuid.end());
        return uuid;
      }
  };
}

#endif
