#include <ctime>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "gallocy/consensus/client.h"
#include "gallocy/consensus/log.h"
#include "gallocy/consensus/server.h"
#include "gallocy/entrypoint.h"
#include "gallocy/http/request.h"
#include "gallocy/http/response.h"
#include "gallocy/models.h"
#include "gallocy/threads.h"
#include "gallocy/utils/logging.h"
#include "gallocy/utils/stringutils.h"


/**
 * Die.
 *
 * :param sc: A custom error to pass alont to ``perror``.
 */
void gallocy::consensus::error_die(const char *sc) {
    perror(sc);
    exit(1);
}


gallocy::http::Response *gallocy::consensus::GallocyServer::route_admin(RouteArguments *args, gallocy::http::Request *request) {
    gallocy::http::Response *response = new (internal_malloc(sizeof(gallocy::http::Response))) gallocy::http::Response();
    response->status_code = 200;
    response->headers["Server"] = "Gallocy-Httpd";
    response->headers["Content-Type"] = "application/json";
    response->body = gallocy_state->to_json().dump();
    args->~RouteArguments();
    internal_free(args);
    return response;
}


gallocy::http::Response *gallocy::consensus::GallocyServer::route_request_vote(RouteArguments *args, gallocy::http::Request *request) {
    gallocy::common::Peer candidate_voted_for = request->peer;
    gallocy::json request_json = request->get_json();
    uint64_t candidate_commit_index = request_json["commit_index"];
    uint64_t candidate_current_term = request_json["term"];
    uint64_t candidate_last_applied = request_json["last_applied"];
    bool granted = gallocy_state->try_grant_vote(
            candidate_voted_for, candidate_commit_index, candidate_current_term, candidate_last_applied);
    gallocy::json response_json = {
        { "term", gallocy_state->get_current_term() },
        { "vote_granted", granted },
    };
    gallocy::http::Response *response = new (internal_malloc(sizeof(gallocy::http::Response))) gallocy::http::Response();
    response->headers["Server"] = "Gallocy-Httpd";
    response->headers["Content-Type"] = "application/json";
    response->status_code = 200;
    response->body = response_json.dump();
    args->~RouteArguments();
    internal_free(args);
    return response;
}


gallocy::http::Response *gallocy::consensus::GallocyServer::route_append_entries(RouteArguments *args, gallocy::http::Request *request) {
    gallocy::common::Peer peer = request->peer;
    gallocy::json request_json = request->get_json();
    gallocy::vector<gallocy::consensus::LogEntry> leader_entries;
    uint64_t leader_commit_index = request_json["leader_commit"];
    uint64_t leader_prev_log_index = request_json["previous_log_index"];
    uint64_t leader_prev_log_term = request_json["previous_log_term"];
    uint64_t leader_term = request_json["term"];

    // DECODE log entries from JSON payload
    for (auto entry_json : request_json["entries"]) {
        // TODO(sholsapp): Implicit conversion issue.
        gallocy::string tmp = entry_json["command"];
        uint64_t term = entry_json["term"];
        gallocy::consensus::Command command(tmp);
        gallocy::consensus::LogEntry entry(command, term);
        leader_entries.push_back(entry);
    }

    bool success = gallocy_state->try_replicate_log(
            peer, leader_entries, leader_commit_index,
            leader_prev_log_index, leader_prev_log_term, leader_term);;

    gallocy::json response_json = {
        { "term", gallocy_state->get_current_term() },
        { "success", success },
    };
    gallocy::http::Response *response = new (internal_malloc(sizeof(gallocy::http::Response))) gallocy::http::Response();
    response->headers["Server"] = "Gallocy-Httpd";
    response->headers["Content-Type"] = "application/json";
    response->status_code = 200;
    response->body = response_json.dump();
    args->~RouteArguments();
    internal_free(args);
    return response;
}


// TODO(sholsapp): This is just an example of a client request... this will
// actually be moved into a signal handler or a terminal.
gallocy::http::Response *gallocy::consensus::GallocyServer::route_request(RouteArguments *args, gallocy::http::Request *request) {
    // PREPARE the commands as log entries
    gallocy::consensus::Command command("hello world");
    gallocy::consensus::LogEntry entry(command, gallocy_state->get_current_term());
    gallocy::vector<gallocy::consensus::LogEntry> entries;
    entries.push_back(entry);

    // SEND the log entries for replication.
    gallocy_client->send_append_entries(entries);

    // RESPOND to the client's request with results.
    gallocy::http::Response *response = new (internal_malloc(sizeof(gallocy::http::Response))) gallocy::http::Response();
    response->headers["Server"] = "Gallocy-Httpd";
    response->headers["Content-Type"] = "application/json";
    response->status_code = 200;
    response->body = "GOOD";
    args->~RouteArguments();
    internal_free(args);
    return response;
}


// TODO(rverdon): This needs an abstraction so that we can swap out the
// underlying transport mechanism for the server *at the same time* as the
// client.

//
// The server socket implementation
//


void *gallocy::consensus::GallocyServer::work() {
    LOG_DEBUG("Starting HTTP server on " << address << ":" << port);

    struct sockaddr_in name;
    int optval = 1;

    server_socket = socket(PF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        gallocy::consensus::error_die("socket");
    }

#ifdef __APPLE__
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
#else
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
#endif

    memset(&name, 0, sizeof(name));
    name.sin_family = AF_INET;
    name.sin_port = htons(port);
    // name.sin_addr.s_addr = htonl(INADDR_ANY);
    name.sin_addr.s_addr = inet_addr(address.c_str());

    if (bind(server_socket, (struct sockaddr *) &name, sizeof(name)) < 0) {
        gallocy::consensus::error_die("bind");
    }

    if (listen(server_socket, 5) < 0) {
        gallocy::consensus::error_die("listen");
    }

    int64_t client_sock = -1;
    struct sockaddr_in client_name;
    uint64_t client_name_len = sizeof(client_name);
    pthread_t newthread;

    while (alive) {
        client_sock = accept(server_socket,
                reinterpret_cast<struct sockaddr *>(&client_name),
                reinterpret_cast<socklen_t *>(&client_name_len));

        if (client_sock == -1) {
            gallocy::consensus::error_die("accept");
        }

        struct RequestContext *ctx =
            new (internal_malloc(sizeof(struct RequestContext))) struct RequestContext;
        ctx->server = this;
        ctx->client_socket = client_sock;
        ctx->client_name = client_name;

        if (get_pthread_create_impl()(&newthread, NULL, handle_entry, reinterpret_cast<void *>(ctx)) != 0) {
            perror("pthread_create1");
        }

        // TODO(sholsapp): This shouldn't block, and we shouldn't just try to
        // join this thread.
        if (get_pthread_join_impl()(newthread, nullptr)) {
            perror("pthread_join1");
        }
    }

    close(server_socket);

    return nullptr;
}


void *gallocy::consensus::GallocyServer::handle_entry(void *arg) {
    struct RequestContext *ctx = reinterpret_cast<struct RequestContext *>(arg);
    void *ret = ctx->server->handle(ctx->client_socket, ctx->client_name);
    ctx->~RequestContext();
    internal_free(ctx);
    return ret;
}


void *gallocy::consensus::GallocyServer::handle(int client_socket, struct sockaddr_in client_name) {
    gallocy::http::Request *request = get_request(client_socket, client_name);
    gallocy::http::Response *response = routes.match(request->uri)(request);

    if (send(client_socket, response->str().c_str(), response->size(), 0) == -1) {
        gallocy::consensus::error_die("send");
    }

    LOG_INFO(request->method
            << " "
            << request->uri
            << " - "
            << "HTTP " << response->status_code
            << " - "
            << inet_ntoa(client_name.sin_addr)
            << " "
            << request->headers["User-Agent"]);

    // Teardown
    request->~Request();
    internal_free(request);
    response->~Response();
    internal_free(response);

    shutdown(client_socket, SHUT_RDWR);
    close(client_socket);

    return nullptr;
}


gallocy::http::Request *gallocy::consensus::GallocyServer::get_request(int client_socket, struct sockaddr_in client_name) {
    gallocy::stringstream request;
    int n;
    char buf[512] = {0};
    n = recv(client_socket, buf, 16, 0);
    request << buf;
    while (n > 0) {
        memset(buf, 0, 512);
        n = recv(client_socket, buf, 512, MSG_DONTWAIT);
        request << buf;
    }
    return new (internal_malloc(sizeof(gallocy::http::Request)))
        gallocy::http::Request(request.str(), gallocy::common::Peer(client_name));
}
