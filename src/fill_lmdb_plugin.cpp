// copyright defined in LICENSE.txt

#include "state_history_lmdb.hpp"

#include "fill_lmdb_plugin.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/iostreams/device/back_inserter.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <fc/exception/exception.hpp>

using namespace abieos;
using namespace appbase;
using namespace std::literals;
namespace lmdb = state_history::lmdb;

using std::enable_shared_from_this;
using std::exception;
using std::make_shared;
using std::make_unique;
using std::map;
using std::max;
using std::min;
using std::optional;
using std::runtime_error;
using std::shared_ptr;
using std::string;
using std::string_view;
using std::to_string;
using std::unique_ptr;
using std::variant;
using std::vector;

namespace asio      = boost::asio;
namespace bio       = boost::iostreams;
namespace bpo       = boost::program_options;
namespace websocket = boost::beast::websocket;

using asio::ip::tcp;
using boost::beast::flat_buffer;
using boost::system::error_code;

std::vector<char> zlib_decompress(input_buffer data) {
    std::vector<char>      out;
    bio::filtering_ostream decomp;
    decomp.push(bio::zlib_decompressor());
    decomp.push(bio::back_inserter(out));
    bio::write(decomp, data.pos, data.end - data.pos);
    bio::close(decomp);
    return out;
}

struct session;

struct fill_lmdb_config {
    string   host;
    string   port;
    string   schema;
    uint32_t db_size_mb  = 0;
    uint32_t skip_to     = 0;
    uint32_t stop_before = 0;
    bool     enable_trim = false;
};

struct fill_lmdb_plugin_impl : std::enable_shared_from_this<fill_lmdb_plugin_impl> {
    shared_ptr<fill_lmdb_config> config = make_shared<fill_lmdb_config>();
    shared_ptr<::session>        session;

    ~fill_lmdb_plugin_impl();
};

struct session : enable_shared_from_this<session> {
    fill_lmdb_plugin_impl*         my = nullptr;
    shared_ptr<fill_lmdb_config>   config;
    lmdb::env                      lmdb_env;
    lmdb::database                 db{lmdb_env};
    tcp::resolver                  resolver;
    websocket::stream<tcp::socket> stream;
    bool                           received_abi    = false;
    bool                           created_trim    = false;
    uint32_t                       head            = 0;
    abieos::checksum256            head_id         = {};
    uint32_t                       irreversible    = 0;
    abieos::checksum256            irreversible_id = {};
    uint32_t                       first           = 0;
    uint32_t                       first_bulk      = 0;
    abi_def                        abi             = {};
    map<string, abi_type>          abi_types       = {};

    session(fill_lmdb_plugin_impl* my, asio::io_context& ioc)
        : my(my)
        , config(my->config)
        , lmdb_env(config->db_size_mb)
        , resolver(ioc)
        , stream(ioc) {

        ilog("connect to lmdb");
        stream.binary(true);
        stream.read_message_max(1024 * 1024 * 1024);
    }

    void start() {
        ilog("connect to ${h}:${p}", ("h", config->host)("p", config->port));
        resolver.async_resolve(
            config->host, config->port, [self = shared_from_this(), this](error_code ec, tcp::resolver::results_type results) {
                callback(ec, "resolve", [&] {
                    asio::async_connect(
                        stream.next_layer(), results.begin(), results.end(), [self = shared_from_this(), this](error_code ec, auto&) {
                            callback(ec, "connect", [&] {
                                stream.async_handshake(config->host, "/", [self = shared_from_this(), this](error_code ec) {
                                    callback(ec, "handshake", [&] { //
                                        start_read();
                                    });
                                });
                            });
                        });
                });
            });
    }

    void start_read() {
        auto in_buffer = make_shared<flat_buffer>();
        stream.async_read(*in_buffer, [self = shared_from_this(), this, in_buffer](error_code ec, size_t) {
            callback(ec, "async_read", [&] {
                if (!received_abi)
                    receive_abi(in_buffer);
                else {
                    if (!receive_result(in_buffer)) {
                        close();
                        return;
                    }
                }
                start_read();
            });
        });
    }

    void receive_abi(const shared_ptr<flat_buffer>& p) {
        auto data = p->data();
        json_to_native(abi, string_view{(const char*)data.data(), data.size()});
        check_abi_version(abi.version);
        abi_types    = create_contract(abi).abi_types;
        received_abi = true;

        lmdb::transaction t{lmdb_env, true};
        load_fill_status(t);
        auto positions = get_positions(t);
        truncate(head + 1);
        t.commit();

        send_request(positions);
    }

    void load_fill_status(lmdb::transaction& t) {
        auto r          = lmdb::get<lmdb::fill_status>(t, db, lmdb::make_fill_status_key(), false);
        head            = r.head;
        head_id         = r.head_id;
        irreversible    = r.irreversible;
        irreversible_id = r.irreversible_id;
        first           = r.first;
    }

    jarray get_positions(lmdb::transaction& t) {
        return {}; // todo
    }

    void write_fill_status(lmdb::transaction& t) {
        put(t, db, lmdb::make_fill_status_key(),
            lmdb::fill_status{
                .head = head, .head_id = head_id, .irreversible = irreversible, .irreversible_id = irreversible_id, .first = first},
            true);
    }

    void truncate(uint32_t block) {
        // todo
    }

    bool receive_result(const shared_ptr<flat_buffer>& p) {
        auto         data = p->data();
        input_buffer bin{(const char*)data.data(), (const char*)data.data() + data.size()};
        check_variant(bin, get_type("result"), "get_blocks_result_v0");

        state_history::get_blocks_result_v0 result;
        bin_to_native(result, bin);

        if (!result.this_block)
            return true;

        bool bulk = false;

        if (config->stop_before && result.this_block->block_num >= config->stop_before) {
            ilog("block ${b}: stop requested", ("b", result.this_block->block_num));
            return false;
        }

        if (result.this_block->block_num <= head)
            ilog("switch forks at block ${b}", ("b", result.this_block->block_num));

        trim();
        ilog("block ${b}", ("b", result.this_block->block_num));

        lmdb::transaction t{lmdb_env, true};
        if (result.this_block->block_num <= head)
            truncate(result.this_block->block_num);
        if (head_id != abieos::checksum256{} && (!result.prev_block || result.prev_block->block_id != head_id))
            throw runtime_error("prev_block does not match");
        if (result.block)
            receive_block(result.this_block->block_num, result.this_block->block_id, *result.block, t);
        if (result.deltas)
            receive_deltas(result.this_block->block_num, *result.deltas, bulk);
        if (result.traces)
            receive_traces(result.this_block->block_num, *result.traces, bulk);

        head            = result.this_block->block_num;
        head_id         = result.this_block->block_id;
        irreversible    = result.last_irreversible.block_num;
        irreversible_id = result.last_irreversible.block_id;
        if (!first)
            first = head;
        write_fill_status(t);

        put(t, db, lmdb::make_received_block_key(result.this_block->block_num), lmdb::received_block{result.this_block->block_id});

        t.commit();
        return true;
    } // receive_result()

    void receive_block(uint32_t block_index, const checksum256& block_id, input_buffer bin, lmdb::transaction& t) {
        state_history::signed_block block;
        bin_to_native(block, bin);
        lmdb::block_info info{
            .block_index       = block_index,
            .block_id          = block_id,
            .timestamp         = block.timestamp,
            .producer          = block.producer,
            .confirmed         = block.confirmed,
            .previous          = block.previous,
            .transaction_mroot = block.transaction_mroot,
            .action_mroot      = block.action_mroot,
            .schedule_version  = block.schedule_version,
            .new_producers     = block.new_producers ? *block.new_producers : state_history::producer_schedule{},
        };
        put(t, db, lmdb::make_block_info_key(block_index), info);
    } // receive_block

    void receive_deltas(uint32_t block_num, input_buffer buf, bool bulk) {
        auto         data = zlib_decompress(buf);
        input_buffer bin{data.data(), data.data() + data.size()};

        auto     num     = read_varuint32(bin);
        unsigned numRows = 0;
        for (uint32_t i = 0; i < num; ++i) {
            check_variant(bin, get_type("table_delta"), "table_delta_v0");
            state_history::table_delta_v0 table_delta;
            bin_to_native(table_delta, bin);

            auto& variant_type = get_type(table_delta.name);
            if (!variant_type.filled_variant || variant_type.fields.size() != 1 || !variant_type.fields[0].type->filled_struct)
                throw std::runtime_error("don't know how to proccess " + variant_type.name);
            auto& type = *variant_type.fields[0].type;

            size_t num_processed = 0;
            for (auto& row : table_delta.rows) {
                if (table_delta.rows.size() > 10000 && !(num_processed % 10000))
                    ilog(
                        "block ${b} ${t} ${n} of ${r} bulk=${bulk}",
                        ("b", block_num)("t", table_delta.name)("n", num_processed)("r", table_delta.rows.size())("bulk", bulk));
                check_variant(row.data, variant_type, 0u);
                // todo
                ++num_processed;
            }
            numRows += table_delta.rows.size();
        }
    } // receive_deltas

    void receive_traces(uint32_t block_num, input_buffer buf, bool bulk) {
        // todo
    }

    void trim() {
        if (!config->enable_trim)
            return;
        auto end_trim = min(head, irreversible);
        if (first >= end_trim)
            return;
        ilog("trim  ${b} - ${e}", ("b", first)("e", end_trim));
        // todo
        ilog("      done");
        first = end_trim;
    }

    void send_request(const jarray& positions) {
        send(jvalue{jarray{{"get_blocks_request_v0"s},
                           {jobject{
                               {{"start_block_num"s}, {to_string(max(config->skip_to, head + 1))}},
                               {{"end_block_num"s}, {"4294967295"s}},
                               {{"max_messages_in_flight"s}, {"4294967295"s}},
                               {{"have_positions"s}, {positions}},
                               {{"irreversible_only"s}, {false}},
                               {{"fetch_block"s}, {true}},
                               {{"fetch_traces"s}, {true}},
                               {{"fetch_deltas"s}, {true}},
                           }}}});
    }

    const abi_type& get_type(const string& name) {
        auto it = abi_types.find(name);
        if (it == abi_types.end())
            throw runtime_error("unknown type "s + name);
        return it->second;
    }

    void send(const jvalue& value) {
        auto bin = make_shared<vector<char>>();
        json_to_bin(*bin, &get_type("request"), value);
        stream.async_write(
            asio::buffer(*bin), [self = shared_from_this(), bin, this](error_code ec, size_t) { callback(ec, "async_write", [&] {}); });
    }

    void check_variant(input_buffer& bin, const abi_type& type, uint32_t expected) {
        auto index = read_varuint32(bin);
        if (!type.filled_variant)
            throw runtime_error(type.name + " is not a variant"s);
        if (index >= type.fields.size())
            throw runtime_error("expected "s + type.fields[expected].name + " got " + to_string(index));
        if (index != expected)
            throw runtime_error("expected "s + type.fields[expected].name + " got " + type.fields[index].name);
    }

    void check_variant(input_buffer& bin, const abi_type& type, const char* expected) {
        auto index = read_varuint32(bin);
        if (!type.filled_variant)
            throw runtime_error(type.name + " is not a variant"s);
        if (index >= type.fields.size())
            throw runtime_error("expected "s + expected + " got " + to_string(index));
        if (type.fields[index].name != expected)
            throw runtime_error("expected "s + expected + " got " + type.fields[index].name);
    }

    template <typename F>
    void catch_and_close(F f) {
        try {
            f();
        } catch (const exception& e) {
            elog("${e}", ("e", e.what()));
            close();
        } catch (...) {
            elog("unknown exception");
            close();
        }
    }

    template <typename F>
    void callback(error_code ec, const char* what, F f) {
        if (ec)
            return on_fail(ec, what);
        catch_and_close(f);
    }

    void on_fail(error_code ec, const char* what) {
        try {
            elog("${w}: ${m}", ("w", what)("m", ec.message()));
            close();
        } catch (...) {
            elog("exception while closing");
        }
    }

    void close() {
        stream.next_layer().close();
        if (my)
            my->session.reset();
    }

    ~session() { ilog("fill-lmdb stopped"); }
}; // session

static abstract_plugin& _fill_lmdb_plugin = app().register_plugin<fill_lmdb_plugin>();

fill_lmdb_plugin_impl::~fill_lmdb_plugin_impl() {
    if (session)
        session->my = nullptr;
}

fill_lmdb_plugin::fill_lmdb_plugin()
    : my(make_shared<fill_lmdb_plugin_impl>()) {}

fill_lmdb_plugin::~fill_lmdb_plugin() {}

void fill_lmdb_plugin::set_program_options(options_description& cli, options_description& cfg) {
    auto op   = cfg.add_options();
    auto clop = cli.add_options();
    op("endpoint,e", bpo::value<string>()->default_value("localhost:8080"), "State-history endpoint to connect to (nodeos)");
    op("schema,s", bpo::value<string>()->default_value("chain"), "Database schema");
    op("trim,t", "Trim history before irreversible");
    clop(
        "set-db-size-mb", bpo::value<uint32_t>(),
        "Increase database size to [arg]. This option will grow the database size limit, but not shrink it");
    clop("skip-to,k", bpo::value<uint32_t>(), "Skip blocks before [arg]");
    clop("stop,x", bpo::value<uint32_t>(), "Stop before block [arg]");
    clop("drop,D", "Drop (delete) schema and tables");
    clop("create,C", "Create schema and tables");
}

void fill_lmdb_plugin::plugin_initialize(const variables_map& options) {
    try {
        auto endpoint           = options.at("endpoint").as<string>();
        auto port               = endpoint.substr(endpoint.find(':') + 1, endpoint.size());
        auto host               = endpoint.substr(0, endpoint.find(':'));
        my->config->host        = host;
        my->config->port        = port;
        my->config->schema      = options["schema"].as<string>();
        my->config->db_size_mb  = options.count("set-db-size-mb") ? options["set-db-size-mb"].as<uint32_t>() : 0;
        my->config->skip_to     = options.count("skip-to") ? options["skip-to"].as<uint32_t>() : 0;
        my->config->stop_before = options.count("stop") ? options["stop"].as<uint32_t>() : 0;
        my->config->enable_trim = options.count("trim");
    }
    FC_LOG_AND_RETHROW()
}

void fill_lmdb_plugin::plugin_startup() {
    my->session = make_shared<session>(my.get(), app().get_io_service());
    my->session->start();
}

void fill_lmdb_plugin::plugin_shutdown() {
    if (my->session)
        my->session->close();
}
