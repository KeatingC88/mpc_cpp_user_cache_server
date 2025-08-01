﻿#include <string>
#include <fstream>
#include <sstream>
#include <cstdlib>

#include "crow_all.h"
#include <tacopie/tacopie>

#include "jwt-cpp/jwt.h"
#include <json.hpp>
using json = nlohmann::json;

#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <vector>
#include <stdexcept>
#include <iostream>

#include <cpp_redis/cpp_redis>

crow::SimpleApp app;
cpp_redis::client client;

std::vector<unsigned char> global_key;
std::vector<unsigned char> global_iv;

void Load_Env_File(const std::string& path = ".env") {
    std::ifstream file(path);

    if (!file.is_open()) {
        std::cerr << "Could not open .env file\n";
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        std::istringstream lineStream(line);
        std::string key, value;

        if (std::getline(lineStream, key, '=') &&
            std::getline(lineStream, value)) {
            #ifdef _WIN32
                _putenv_s(key.c_str(), value.c_str());
            #else
                setenv(key.c_str(), value.c_str(), 1);
            #endif
        }
    }
}

std::vector<unsigned char> Base64_Decoder(const std::string& input) {
    BIO* bio = BIO_new_mem_buf(input.data(), input.length());
    BIO* b64 = BIO_new(BIO_f_base64());
    bio = BIO_push(b64, bio);
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);

    std::vector<unsigned char> output(input.length());
    int len = BIO_read(bio, output.data(), static_cast<int>(input.length()));
    output.resize(len > 0 ? len : 0);

    BIO_free_all(bio);
    return output;
}

std::vector<unsigned char> SHA256(const std::string& input) {
    std::vector<unsigned char> hash(SHA256_DIGEST_LENGTH);
    SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), hash.data());
    return hash;
}

std::string AES256_Decryptor(const std::string& base64_ciphertext) {
    if (global_key.size() != 32 || global_iv.size() != 16) {
        throw std::runtime_error("Key must be 32 bytes and IV must be 16 bytes");
    }

    std::vector<unsigned char> ciphertext = Base64_Decoder(base64_ciphertext);
    std::vector<unsigned char> plaintext(ciphertext.size() + EVP_MAX_BLOCK_LENGTH);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("Failed to create context");

    int len = 0, plaintext_len = 0;

    if (!EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, global_key.data(), global_iv.data()))
        throw std::runtime_error("DecryptInit failed");

    if (!EVP_DecryptUpdate(ctx, plaintext.data(), &len, ciphertext.data(), ciphertext.size()))
        throw std::runtime_error("DecryptUpdate failed");

    plaintext_len = len;

    if (!EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &len))
        throw std::runtime_error("DecryptFinal failed");

    plaintext_len += len;
    plaintext.resize(plaintext_len);
    EVP_CIPHER_CTX_free(ctx);

    return std::string(plaintext.begin(), plaintext.end());
}

int Authenticate_JWT_Claims(const std::string& JWT) {
    try {
        const char* JWT_ISSUER_KEY = std::getenv("JWT_ISSUER_KEY");
        const char* JWT_CLIENT_KEY = std::getenv("JWT_CLIENT_KEY");
        const char* JWT_CLIENT_ADDRESS = std::getenv("JWT_CLIENT_ADDRESS");

        auto decoded = jwt::decode(JWT);
        auto payload_json = nlohmann::json::parse(decoded.get_payload());

        for (auto it = payload_json.begin(); it != payload_json.end(); ++it) {
            if (it.value().is_string()) {

                if (it.key() == "aud" && AES256_Decryptor(it.value()) != JWT_CLIENT_KEY) {
                    return 0;
                }
                else if (it.key() == "iss" && AES256_Decryptor(it.value()) != JWT_ISSUER_KEY) {
                    return 0;
                }

                if (AES256_Decryptor(it.value()) == JWT_CLIENT_ADDRESS) {
                    return 1;
                }

            } else if (std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) >= it.value()) {
                return 0;
            }
        }
        return 1;
    } catch (const std::exception& e) {
        return 0;
    }
}

struct CORS {
    struct context {};

    void before_handle(crow::request& req, crow::response& res, context&) {
        // Allow preflight (OPTIONS) requests to continue
        if (req.method == crow::HTTPMethod::Options) {
            res.code = 204; // No Content
            add_cors_headers(res);
            res.end();
        }
    }

    void after_handle(crow::request& req, crow::response& res, context&) {
        add_cors_headers(res);
    }

    void add_cors_headers(crow::response& res) {
        res.add_header("Access-Control-Allow-Origin", "*");
        res.add_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        res.add_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
    }
};

int main()
{
    crow::App<CORS> app;
    Load_Env_File();
    const char* HOST_IP_ADDRESS = std::getenv("HOST_IP_ADDRESS");
    const char* HOST_PORT_ADDRESS = std::getenv("HOST_PORT_ADDRESS");
    const char* REDIS_HOST_ADDRESS = std::getenv("REDIS_HOST_ADDRESS");
    const char* REDIS_PORT_ADDRESS = std::getenv("REDIS_PORT_ADDRESS");

    const char* key_env = std::getenv("ENCRYPTION_KEY");
    const char* iv_env = std::getenv("ENCRYPTION_IV");

    if (!key_env || !iv_env || std::strlen(key_env) != 32 || std::strlen(iv_env) != 16) {
        std::cerr << "Invalid key or IV in environment variables." << std::endl;
        return 1;
    }

    std::string key_str = key_env;
    std::string iv_str = iv_env;

    global_key = SHA256(key_str);
    global_iv = std::vector<unsigned char>(iv_str.begin(), iv_str.end());

    client.connect(REDIS_HOST_ADDRESS, std::stoi(REDIS_PORT_ADDRESS), [](const std::string& host, std::size_t port, cpp_redis::client::connect_state status) {
        if (status == cpp_redis::client::connect_state::dropped) {
            std::cerr << "Client disconnected from " << host << ":" << port << std::endl;
        }
    });

    CROW_ROUTE(app, "/set/user").methods(crow::HTTPMethod::Post)([](const crow::request& req) {
        auto body = crow::json::load(req.body);

        if (!body || 
            !body.has("id") ||
            !body.has("token") ||
            !body.has("online_status") ||
            !body.has("custom_lbl") ||
            !body.has("name") ||
            !body.has("created_on") ||
            !body.has("avatar_url_path") ||
            !body.has("avatar_title") ||
            !body.has("language_code") ||
            !body.has("region_code") ||
            !body.has("login_on") ||
            !body.has("logout_on") ||
            !body.has("login_type") ||
            !body.has("account_type") ||
            !body.has("email_address")
            ) {
            return crow::response(400, "Error: 1");//Something is missing from the condition.
        }

        std::string JWT = body["token"].s();
        if (!Authenticate_JWT_Claims(JWT)) {
            return crow::response(400, "Error: 2");//Something is incorrect in the JWToken.
        }

        try {

            std::string encrypted_user_id = body["id"].s();
            std::string decrypted_user_id = AES256_Decryptor(encrypted_user_id);

            client.ltrim(decrypted_user_id, 1, 0);

            client.rpush(decrypted_user_id, { 
                encrypted_user_id, 
                body["online_status"].s(),
                body["custom_lbl"].s(),
                body["name"].s(),
                body["created_on"].s(),
                body["avatar_url_path"].s(),
                body["avatar_title"].s(),
                body["language_code"].s(),
                body["region_code"].s(),
                body["login_on"].s(),
                body["logout_on"].s(),
                body["login_type"].s(),
                body["account_type"].s(),
                body["email_address"].s()
            });

            client.commit();

            return crow::response(200, decrypted_user_id);

        } catch (const std::exception& e) {

            return crow::response(400, "Error: 3");//Try catch failed.

        }
    });

    CROW_ROUTE(app, "/get/user").methods(crow::HTTPMethod::Post)([](const crow::request& req) {
        auto body = crow::json::load(req.body);

        if (!body || 
            !body.has("id") || 
            !body.has("token")
        ) {
            return crow::response(400, "Error 3");//Something is missing from the condition.
        }

        std::string JWT = body["token"].s();
        if (!Authenticate_JWT_Claims(JWT)) {
            return crow::response(400, "Error 4");//Something is incorrect in the JWToken.
        }

        try {
            std::string encrypted_user_id = body["id"].s();
            std::string decrypted_user_id = AES256_Decryptor(encrypted_user_id);

            auto get_reply_future = client.lrange(decrypted_user_id, 0, -1);
            client.commit();

            cpp_redis::reply reply = get_reply_future.get();

            if (reply.is_null() || reply.is_array() == false) {
                return crow::response(404, "Error 5");//Reply from the database is incorrect.
            }

            json j_object;

            size_t index = 0;

            for (const auto& element : reply.as_array()) {
                std::string key;
                switch (index) {
                    case 0: key = "id"; break;
                    case 1: key = "online_status"; break;
                    case 2: key = "custom_lbl"; break;
                    case 3: key = "name"; break;
                    case 4: key = "created_on"; break;
                    case 5: key = "avatar_url_path"; break;
                    case 6: key = "avatar_title"; break;
                    case 7: key = "language_code"; break;
                    case 8: key = "region_code"; break;
                    case 9: key = "login_on"; break;
                    case 10: key = "logout_on"; break;
                    case 11: key = "login_type"; break;
                    case 12: key = "account_type"; break;
                    case 13: key = "email_address"; break;
                    default: key = "unknown_" + std::to_string(index); break;
                }

                if (element.is_string()) {
                    j_object[key] = element.as_string();
                }
                else {
                    j_object[key] = nullptr;
                }

                ++index;
            }

            return crow::response(200, j_object.dump());

        } catch (const std::exception& e) {

            return crow::response(500, e.what());

        }
    });

    CROW_ROUTE(app, "/get/users").methods(crow::HTTPMethod::Post)([](const crow::request& req) {
        auto body = crow::json::load(req.body);

        if (!body ||
            !body.has("token")
            ) {

            return crow::response(400, "Error 6");// missing conditions.

        }

        std::string JWT = body["token"].s();
        if (!Authenticate_JWT_Claims(JWT)) {
            return crow::response(400, "Error 7");//Something is incorrect in the JWToken.
        }

        try {

            std::vector<std::string> all_keys;
            std::size_t cursor = 0;
            std::size_t count = 100;
            json result_json;

            do {
                auto future_reply = client.scan(cursor, count);
                client.sync_commit();
                cpp_redis::reply reply = future_reply.get();

                if (!reply.is_array() || reply.as_array().size() != 2) {
                    std::cerr << "Error 8";//Scan Issue with future_reply.get()
                    break;
                }

                const auto& reply_arr = reply.as_array();
                cursor = std::stoull(reply_arr[0].as_string());

                const auto& keys_array = reply_arr[1].as_array();
                for (const auto& k : keys_array) {
                    if (k.is_string()) {
                        all_keys.push_back(k.as_string());
                    }
                }
            } while (cursor != 0);

            for (const auto& key : all_keys) {
                auto lrange_future = client.lrange(key, 0, -1);
                client.sync_commit();
                cpp_redis::reply lrange_reply = lrange_future.get();

                if (!lrange_reply.is_array()) continue;

                json list_items = json::array();
                for (const auto& item : lrange_reply.as_array()) {
                    if (item.is_string()) {
                        list_items.push_back(item.as_string());
                    }
                    else {
                        list_items.push_back(nullptr);
                    }
                }

                result_json[key] = list_items;
            }

            return crow::response(200, result_json.dump(2));

        } catch (const std::exception& e) {

            return crow::response(500, e.what());

        }
    });

    app.bindaddr(HOST_IP_ADDRESS).port(std::stoi(HOST_PORT_ADDRESS)).multithreaded().run();
}