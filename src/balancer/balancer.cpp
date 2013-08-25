#include <bitcoin/bitcoin.hpp>
#include <obelisk/zmq_message.hpp>

#include <vector>

#define HEARTBEAT_LIVENESS  3       //  3-5 is reasonable
#define HEARTBEAT_INTERVAL  1000    //  msecs

#include "config.hpp"

#define LOG_BALANCER "balancer"

using namespace bc;

static void s_version_assert(int want_major, int want_minor)
{
    int major, minor, patch;
    zmq_version (&major, &minor, &patch);
    if (major < want_major
    || (major == want_major && minor < want_minor)) {
        std::cout << "Current 0MQ version is " << major << "." << minor << std::endl;
        std::cout << "Application needs at least " << want_major << "." << want_minor
              << " - cannot continue" << std::endl;
        exit (EXIT_FAILURE);
    }
}

//  Return current system clock as milliseconds
static int64_t s_clock(void)
{
#if (defined (__WINDOWS__))
    SYSTEMTIME st;
    GetSystemTime (&st);
    return (int64_t) st.wSecond * 1000 + st.wMilliseconds;
#else
    struct timeval tv;
    gettimeofday (&tv, NULL);
    return (int64_t) (tv.tv_sec * 1000 + tv.tv_usec / 1000);
#endif
}

//  This defines one active worker in our worker queue

typedef struct {
    std::string identity;           //  Address of worker
    int64_t     expiry;             //  Expires at this time
} worker_t;

//  Insert worker at end of queue, reset expiry
//  Worker must not already be in queue
void s_worker_append(std::vector<worker_t>& queue, std::string& identity)
{
    bool found = false;
    for (auto it = queue.begin(); it < queue.end(); ++it)
    {
        if (it->identity.compare(identity) == 0)
        {
            std::cout << "E: duplicate worker identity " << identity << std::endl;
            found = true;
            break;
        }
    }
    if (!found)
    {
        worker_t worker;
        worker.identity = identity;
        worker.expiry = s_clock() + HEARTBEAT_INTERVAL * HEARTBEAT_LIVENESS;
        queue.push_back(worker);
    }
}

//  Remove worker from queue, if present
void s_worker_delete(std::vector<worker_t>& queue, std::string& identity)
{
    for (auto it = queue.begin(); it < queue.end(); ++it)
    {
        if (it->identity.compare(identity) == 0)
        {
            it = queue.erase(it);
            break;
        }
    }
}

//  Reset worker expiry, worker must be present
void s_worker_refresh(std::vector<worker_t>& queue, std::string& identity)
{
    bool found = false;
    for (auto it = queue.begin(); it < queue.end(); ++it)
    {
        if (it->identity.compare(identity) == 0)
        {
           it->expiry = s_clock() + HEARTBEAT_INTERVAL * HEARTBEAT_LIVENESS;
           found = true;
           break;
        }
    }
    if (!found)
    {
       std::cout << "E: worker " << identity << " not ready" << std::endl;
    }
}

//  Pop next available worker off queue, return identity
std::string s_worker_dequeue(std::vector<worker_t>& queue)
{
    assert(queue.size());
    std::string identity = queue[0].identity;
    queue.erase(queue.begin());
    return identity;
}

//  Look for & kill expired workers
void s_queue_purge(std::vector<worker_t>& queue)
{
    int64_t clock = s_clock();
    for (auto it = queue.begin(); it < queue.end(); ++it)
    {
        if (clock > it->expiry)
        {
           it = queue.erase(it) - 1;
        }
    }
}

std::string encode_uuid(const data_chunk& data)
{
    static char hex_char[] = "0123456789ABCDEF";
    BITCOIN_ASSERT(data.size() == 17);
    BITCOIN_ASSERT(data[0] == 0x00);
    std::string uuid(33, 0x00);
    uuid[0] = '@';
    for (size_t i = 0; i < 16; ++i)
    {
        uuid[i * 2 + 1] = hex_char[data[i + 1] >> 4];
        uuid[i * 2 + 2] = hex_char[data[i + 1] & 15];
    }
    return uuid;
}


data_chunk decode_uuid(const std::string& uuid)
{
    static char hex_to_bin[128] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, /* */
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, /* */
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, /* */
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9,-1,-1,-1,-1,-1,-1, /* 0..9 */
        -1,10,11,12,13,14,15,-1,-1,-1,-1,-1,-1,-1,-1,-1, /* A..F */
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, /* */
        -1,10,11,12,13,14,15,-1,-1,-1,-1,-1,-1,-1,-1,-1, /* a..f */
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1  /* */
    };
    BITCOIN_ASSERT(uuid.size() == 33);
    BITCOIN_ASSERT(uuid[0] == '@');
    data_chunk data(17);
    data[0] = 0x00;
    for (size_t i = 0; i < 16; ++i)
        data[i + 1] =
            (hex_to_bin[uuid[i * 2 + 1] & 127] << 4) +
               (hex_to_bin[uuid[i * 2 + 2] & 127]);
    return data;
}

typedef std::vector<worker_t> worker_queue;

void forward_request(zmq::socket_t& frontend, zmq::socket_t& backend,
    worker_queue& queue)
{
    // Get client request.
    zmq_message msg_in;
    msg_in.recv(frontend);
    const data_stack& in_parts = msg_in.parts();

    if (in_parts.size() != 6)
    {
        log_warning(LOG_BALANCER) << "Wrong sized message";
        return;
    }
    // First item is client's identity.
    if (in_parts[0].size() != 17)
    {
        log_warning(LOG_BALANCER) << "Client UUID malformed";
        return;
    }
    // Second item is worker identity or nothing.
    if (in_parts[1].size() != 17 && !in_parts[1].empty())
    {
        log_warning(LOG_BALANCER) << "Worker UUID malformed";
        return;
    }

    // We now deconstruct the request message to the backend
    // which looks like:
    //   [CLIENT UUID]
    //   [WORKER UUID]
    //   ...
    // And create a new message that looks like:
    //   [WORKER UUID]
    //   [CLIENT UUID]
    //   ...
    // Before sending it to the worker.

    // This is so the client can specify a worker to send their
    // request specifically to.

    zmq_message msg_out;
    // If second frame is nothing, then we select a random worker.
    if (in_parts[1].empty())
    {
        // Route to next worker
        std::string worker_identity = s_worker_dequeue(queue);
        msg_out.append(decode_uuid(worker_identity));
    }
    else
    {
        // Route to client's preferred worker.
        msg_out.append(in_parts[1]);
    }
    msg_out.append(in_parts[0]);
    // Copy the remaining data.
    for (auto it = in_parts.begin() + 2; it != in_parts.end(); ++it)
        msg_out.append(*it);
    msg_out.send(backend);
}

void passback_response(zmq::socket_t& backend, zmq::socket_t& frontend)
{
}

int main(int argc, char** argv)
{
    s_version_assert(2, 1);
    config_map_type config;
    if (argc == 2)
    {
        std::cout << "Using config file: " << argv[1] << std::endl;
        load_config(config, argv[1]);
    }
    else
        load_config(config, "balancer.cfg");

    // Prepare our context and sockets
    zmq::context_t context(1);
    zmq::socket_t frontend(context, ZMQ_ROUTER);
    zmq::socket_t backend(context, ZMQ_ROUTER);
    // For clients
    frontend.bind(config["frontend"].c_str());
    // For workers
    backend.bind(config["backend"].c_str());

    // Queue of available workers
    worker_queue queue;
    // Send out heartbeats at regular intervals
    int64_t heartbeat_at = s_clock() + HEARTBEAT_INTERVAL;

    while (1)
    {
        zmq::pollitem_t items[] = {
            { backend,  0, ZMQ_POLLIN, 0 },
            { frontend, 0, ZMQ_POLLIN, 0 }
        };

        //  Poll frontend only if we have available workers
        if (queue.size())
            zmq::poll(items, 2, HEARTBEAT_INTERVAL * 1000);
        else
            zmq::poll(items, 1, HEARTBEAT_INTERVAL * 1000);

        // Handle worker activity on backend
        if (items [0].revents & ZMQ_POLLIN)
        {
            zmq_message msg_in;
            msg_in.recv(backend);
            const data_stack& in_parts = msg_in.parts();
            BITCOIN_ASSERT(in_parts.size() == 2 || in_parts.size() == 6);
            std::string identity = encode_uuid(in_parts[0]);

            // Return reply to client if it's not a control message
            if (in_parts.size() == 2)
            {
                std::string command(in_parts[1].begin(), in_parts[1].end());
                log_info(LOG_BALANCER) << "command: " << command;
                if (command == "READY")
                {
                    s_worker_delete(queue, identity);
                    s_worker_append(queue, identity);
                }
                else if (command == "HEARTBEAT")
                {
                    s_worker_refresh(queue, identity);
                }
                else
                    log_error(LOG_BALANCER)
                        << "Invalid command from " << identity;
            }
            else if (in_parts.size() == 6)
            {
                // We now deconstruct the request message to the frontend
                // which looks like:
                //   [WORKER UUID]
                //   [CLIENT UUID]
                //   ...
                // And create a new message that looks like:
                //   [CLIENT UUID]
                //   [WORKER UUID]
                //   ...
                // Before sending it to the backend.

                // This is so the client will know which worker
                // responded to their request.

                BITCOIN_ASSERT(in_parts.size() == 6);
                zmq_message msg_out;
                BITCOIN_ASSERT(in_parts[1].size() == 17);
                msg_out.append(in_parts[1]);
                BITCOIN_ASSERT(in_parts[0].size() == 17);
                msg_out.append(in_parts[0]);
                for (auto it = in_parts.begin() + 2; it != in_parts.end(); ++it)
                    msg_out.append(*it);
                BITCOIN_ASSERT(in_parts.size() == msg_out.parts().size());
                msg_out.send(frontend);
                // Add worker back to available pool of workers.
                s_worker_append(queue, identity);
            }
        }
        if (items [1].revents & ZMQ_POLLIN)
            forward_request(frontend, backend, queue);

        //  Send heartbeats to idle workers if it's time
        if (s_clock() > heartbeat_at)
        {
            for (auto it = queue.begin(); it < queue.end(); ++it)
            {
                zmq_message msg;
                msg.append(decode_uuid(it->identity));
                std::string command = "HEARTBEAT";
                msg.append(data_chunk(command.begin(), command.end()));
                msg.send(backend);
            }
            heartbeat_at = s_clock() + HEARTBEAT_INTERVAL;
        }
        s_queue_purge(queue);
    }
    //  We never exit the main loop
    //  But pretend to do the right shutdown anyhow
    queue.clear();
    return 0;
}
