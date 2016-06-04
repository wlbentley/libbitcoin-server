/**
 * Copyright (c) 2011-2015 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin-server.
 *
 * libbitcoin-server is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Affero General Public License with
 * additional permissions to the one published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version. For more information see LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/server/workers/query_worker.hpp>

#include <functional>
#include <string>
#include <boost/thread.hpp>
#include <bitcoin/protocol.hpp>
#include <bitcoin/server/interface/address.hpp>
#include <bitcoin/server/interface/blockchain.hpp>
#include <bitcoin/server/interface/protocol.hpp>
#include <bitcoin/server/interface/transaction_pool.hpp>
#include <bitcoin/server/messages/incoming.hpp>
#include <bitcoin/server/messages/outgoing.hpp>
#include <bitcoin/server/server_node.hpp>

namespace libbitcoin {
namespace server {

using std::placeholders::_1;
using std::placeholders::_2;
using namespace bc::protocol;

query_worker::query_worker(zmq::authenticator& authenticator,
    server_node& node, bool secure)
  : worker(node.thread_pool()),
    secure_(secure),
    settings_(node.server_settings()),
    node_(node),
    address_notifier_(node),
    authenticator_(authenticator)
{
    // The same interface is attached to the secure and public interfaces.
    attach_interface();
}

// Implement worker as a router.
// v2 libbitcoin-client DEALER does not add delimiter frame.
// The router drops messages for lost peers (query service) and high water.
void query_worker::work()
{
    zmq::socket router(authenticator_, zmq::socket::role::router);

    // Connect socket to the worker endpoint.
    if (!started(connect(router)))
        return;

    zmq::poller poller;
    poller.add(router);

    while (!poller.terminated() && !stopped())
    {
        if (poller.wait().contains(router.id()))
            query(router);
    }

    // Disconnect the socket and exit this thread.
    finished(disconnect(router));
}

// Connect/Disconnect.
//-----------------------------------------------------------------------------

bool query_worker::connect(zmq::socket& router)
{
    const auto security = secure_ ? "secure" : "public";
    const auto& endpoint = secure_ ? query_service::secure_worker :
        query_service::public_worker;

    auto ec = router.connect(endpoint);

    if (ec)
    {
        log::error(LOG_SERVER)
            << "Failed to connect " << security << " query worker to "
            << endpoint << " : " << ec.message();
        return false;
    }

    log::debug(LOG_SERVER)
        << "Connected " << security << " query worker to " << endpoint;
    return true;
}

bool query_worker::disconnect(zmq::socket& router)
{
    const auto security = secure_ ? "secure" : "public";

    if (!router.stop())
    {
        log::error(LOG_SERVER)
            << "Failed to disconnect " << security << " query worker.";
        return false;
    }

    // Don't log stop success.
    return true;
}

// Query Execution.
//-----------------------------------------------------------------------------

// Because the socket is a router we may simply drop invalid queries.
// As a single thread worker this router should not reach high water.
// If we implemented as a replier we would need to always provide a response.
void query_worker::query(zmq::socket& router)
{
    // TODO: rewrite the serial blockchain interface to avoid callbacks.
    const auto sender = [&router](outgoing&& response)
    {
        const auto ec = response.send(router);

        if (ec)
            log::warning(LOG_SERVER)
                << "Failed to send query response to " << response.address()
                << ec.message();
    };

    incoming request;
    const auto ec = request.receive(router);

    if (ec)
    {
        log::debug(LOG_SERVER)
            << "Failed to receive query from " << request.address()
            << ec.message();

        // Because the query did not parse this is likely to be misaddressed.
        sender(outgoing(request, ec));
        return;
    }

    if (settings_.log_requests)
    {
        log::info(LOG_SERVER)
            << "Query " << request.command << " from " << request.address();
    }

    // Locate the request handler for this command.
    const auto handler = command_handlers_.find(request.command);

    if (handler == command_handlers_.end())
    {
        log::debug(LOG_SERVER)
            << "Invalid query command from " << request.address();

        sender(outgoing(request, error::not_found));
        return;
    }

    // Execute the request and forward result to queue.
    handler->second(request, sender);
}

// Query Interface.
// ----------------------------------------------------------------------------

// Class and method names must match protocol expectations (do not change).
#define ATTACH(class_name, method_name, instance) \
    attach(#class_name "." #method_name, \
        std::bind(&bc::server::class_name::method_name, \
            std::ref(instance), _1, _2));

void query_worker::attach(const std::string& command,
    command_handler handler)
{
    command_handlers_[command] = handler;
}

// Class and method names must match protocol expectations (do not change).
void query_worker::attach_interface()
{
    // TODO: add total_connections to client.
    ATTACH(protocol, total_connections, node_);
    ATTACH(protocol, broadcast_transaction, node_);
    ATTACH(transaction_pool, validate, node_);
    ATTACH(transaction_pool, fetch_transaction, node_);

    // TODO: add fetch_spend to client.
    // TODO: add fetch_block_transaction_hashes to client.
    ATTACH(blockchain, fetch_spend, node_);
    ATTACH(blockchain, fetch_transaction, node_);
    ATTACH(blockchain, fetch_last_height, node_);
    ATTACH(blockchain, fetch_block_header, node_);
    ATTACH(blockchain, fetch_block_height, node_);
    ATTACH(blockchain, fetch_transaction_index, node_);
    ATTACH(blockchain, fetch_stealth, node_);
    ATTACH(blockchain, fetch_history, node_);
    ATTACH(blockchain, fetch_block_transaction_hashes, node_);

    // address.fetch_history was present in v1 (obelisk) and v2 (server).
    // address.fetch_history was called by client v1 (sx) and v2 (bx).
    ////ATTACH(endpoint, address, fetch_history, node_);
    ATTACH(address, fetch_history2, node_);

    // TODO: add renew to client.
    ATTACH(address, renew, address_notifier_);
    ATTACH(address, subscribe, address_notifier_);
}

#undef ATTACH

} // namespace server
} // namespace libbitcoin
