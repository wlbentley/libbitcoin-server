/**
 * Copyright (c) 2011-2017 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/server/interface/address.hpp>

#include <cstdint>
#include <functional>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/server/messages/message.hpp>
#include <bitcoin/server/server_node.hpp>

namespace libbitcoin {
namespace server {

using namespace std::placeholders;
using namespace bc::chain;
using namespace bc::wallet;

void address::subscribe2(server_node& node, const message& request,
    send_handler handler)
{
    binary prefix_filter;

    if (!unwrap_subscribe2_args(prefix_filter, request))
    {
        handler(message(request, error::bad_stream));
        return;
    }

    // May cause a notification to fire in addition to the response below.
    const auto ec = node.subscribe_address(request.route(), request.id(),
        prefix_filter, false);

    handler(message(request, ec));
}

void address::unsubscribe2(server_node& node, const message& request,
    send_handler handler)
{
    binary prefix_filter;

    if (!unwrap_subscribe2_args(prefix_filter, request))
    {
        handler(message(request, error::bad_stream));
        return;
    }

    // May cause a notification to fire in addition to the response below.
    const auto ec = node.subscribe_address(request.route(), request.id(),
        prefix_filter, true);

    handler(message(request, ec));
}

bool address::unwrap_subscribe2_args(binary& prefix_filter,
    const message& request)
{
    // [ prefix_bitsize:1 ]
    // [ prefix_blocks:...]
    const auto& data = request.data();

    if (data.empty())
        return false;

    const auto bit_length = data[0];
    const auto byte_length = binary::blocks_size(bit_length);

    if (byte_length > short_hash_size || (data.size() - 1) != byte_length)
        return false;

    const data_chunk bytes({ data.begin() + 1, data.end() });
    prefix_filter = binary(bit_length, bytes);
    return true;
}

} // namespace server
} // namespace libbitcoin
