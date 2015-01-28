#include <tools/log.hpp>
#include <zmqpp/curve.hpp>
#include "RemoteControl.hpp"
#include "kernel.hpp"
#include <boost/regex.hpp>
#include <cassert>
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/property_tree/ptree_serialization.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <tools/XmlPropertyTree.hpp>

using namespace Leosac;

RemoteControl::RemoteControl(zmqpp::context &ctx,
        Kernel &kernel,
        const boost::property_tree::ptree &cfg) :
        kernel_(kernel),
        socket_(ctx, zmqpp::socket_type::router),
        auth_(ctx),
        context_(ctx)
{
    auth_.configure_curve("CURVE_ALLOW_ANY");
    process_config(cfg);
}

void RemoteControl::process_config(const boost::property_tree::ptree &cfg)
{
    int port = cfg.get<int>("port");

    secret_key_ = cfg.get<std::string>("secret_key");
    public_key_ = cfg.get<std::string>("public_key");

    INFO("Binding RemoteControl socket to port " << port);
    INFO("Use private key {" << secret_key_ << "} and public key {" << public_key_ << "}");

    command_handlers_["MODULE_CONFIG"] = std::bind(&RemoteControl::handle_module_config, this, std::placeholders::_1,
            std::placeholders::_2);

    command_handlers_["MODULE_LIST"] = std::bind(&RemoteControl::handle_module_list, this, std::placeholders::_1,
            std::placeholders::_2);

    command_handlers_["SYNC_FROM"] = std::bind(&RemoteControl::handle_sync_from, this, std::placeholders::_1,
            std::placeholders::_2);

    command_handlers_["SAVE"] = std::bind(&RemoteControl::handle_save, this, std::placeholders::_1,
            std::placeholders::_2);

    command_handlers_["GENERAL_CONFIG"] = std::bind(&RemoteControl::handle_general_config, this, std::placeholders::_1,
            std::placeholders::_2);

    socket_.set(zmqpp::socket_option::curve_server, true);
    socket_.set(zmqpp::socket_option::curve_secret_key, secret_key_);
    socket_.set(zmqpp::socket_option::curve_public_key, public_key_);
    socket_.bind("tcp://*:" + std::to_string(port));
}

void RemoteControl::handle_msg()
{
    zmqpp::message msg;
    zmqpp::message rep;
    std::string source;
    std::string frame1;
    socket_.receive(msg);

    assert(msg.parts() > 1);
    DEBUG("PARTS = " << msg.parts());
    msg >> source;
    rep << source;

    msg >> frame1;
    DEBUG("Cmd = {" << frame1 << "}");

    if (command_handlers_.find(frame1) != command_handlers_.end())
    {
        auto h = command_handlers_[frame1];

        bool ret = h(&msg, &rep);
        if (!ret)
        {
            // if the handler fails, that means the source message was incorrect.
            // therefore, the response shall be empty (except for the zmq id)
            assert(rep.parts() == 1);
            rep << "KO" << "Malformed message: " << frame1;
            NOTICE("Received malformed message on Remote Control Interface. Message type was " << frame1);
        }
    }
    else
    {
        rep << "KO" << "UNKOWN MESSAGE";
        WARN("UNKNOWN MESSAGE ON REMOTE CONTROL INTERFACE");
    }

    DEBUG("Sending response, " << rep.parts() << " frames");
    socket_.send(rep);
}

void RemoteControl::module_list(zmqpp::message *message_out)
{
    assert(message_out);
    for (const std::string &s : kernel_.module_manager().modules_names())
    {
        *message_out << s;
    }
}

void RemoteControl::module_config(const std::string &module, ConfigManager::ConfigFormat cfg_format, zmqpp::message *message_out)
{
    assert(message_out);

    // we need to make sure the module's name exist.
    std::vector<std::string> modules_names = kernel_.module_manager().modules_names();
    if (std::find(modules_names.begin(), modules_names.end(), module) != modules_names.end())
    {
        zmqpp::socket sock(context_, zmqpp::socket_type::req);
        sock.connect("inproc://module-" + module);

        // ask for a binary dump
        bool ret = sock.send(zmqpp::message() << "DUMP_CONFIG" << cfg_format);


        assert(ret);
        zmqpp::message rep;

        sock.receive(rep);
        *message_out << "OK";
        *message_out << module;

        // extract data from received configuration
        // fixme this is poor code and involve lots of copying.
        while (rep.remaining())
        {
            std::string tmp;
            rep >> tmp;
            *message_out << tmp;
        }
    }
    else
    {
        // if module with this name is not found
        ERROR("RemoteControl: Cannot retrieve local module configuration for {" << module << "}" <<
                "The module appears to not be loaded.");
        *message_out << "KO" << "Module not loaded, so config not available";
    }
}

void RemoteControl::general_config(ConfigManager::ConfigFormat cfg_format, zmqpp::message *msg_out)
{
    assert(msg_out);

    auto cfg = kernel_.config_manager().get_exportable_general_config();

    if (cfg_format == ConfigManager::ConfigFormat::BOOST_ARCHIVE)
    {
        std::ostringstream oss;
        boost::archive::text_oarchive archive(oss);
        boost::property_tree::save(archive, cfg, 1);
        msg_out->add("OK");
        msg_out->add(oss.str());
    }
    else
    {
        msg_out->add("OK");
        msg_out->add(Tools::propertyTreeToXml(cfg));
    }
}

static bool validate_endpoint(const std::string &endpoint)
{
    static const boost::regex r_endpoint("tcp://((\\d{1,3}\\.){3}\\d{1,3}):\\d+");
    return regex_match(endpoint, r_endpoint);
}

void RemoteControl::sync_from(const std::string &endpoint,
        const std::string &remote_server_pk,
        zmqpp::message *message_out)
{
    assert(message_out);
    zmqpp::socket sock(context_, zmqpp::socket_type::dealer);

    auto kp = zmqpp::curve::generate_keypair();
    sock.set(zmqpp::socket_option::curve_secret_key, kp.secret_key);
    sock.set(zmqpp::socket_option::curve_public_key, kp.public_key);
    sock.set(zmqpp::socket_option::curve_server_key, remote_server_pk);
    sock.set(zmqpp::socket_option::linger, 0);

    if (validate_endpoint(endpoint))
    {
        INFO("Attempting to fetch config from {" << endpoint << "}");
        sock.connect(endpoint);

        INFO("Remote Control module entering blocking mode...");

        // list of module to stop
        std::list<std::string> stop_list;

        // list of module to start.
        std::list<std::string> start_list;

        if (gather_remote_config(sock, start_list, stop_list))
        {
            for (const std::string &m : stop_list)
            {
                kernel_.module_manager().stopModule(m);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            for (const std::string &m : start_list)
            {
                if (!kernel_.module_manager().initModule(m))
                {
                    // maybe it wasn't preloaded in the module manager.
                    // attempt to load dll and try again.
                    kernel_.module_manager().loadModule(kernel_.config_manager().load_config(m));
                    bool r = kernel_.module_manager().initModule(m);
                    assert(r);
                }
            }

            INFO("Remote Control resuming normal operation");
            *message_out << "OK";
        }
        else
        {
            *message_out << "KO";
            *message_out << "Failed to synchronise configuration.";
        }
    }
    else
    {
        *message_out << "KO";
        *message_out << "Endpoint seems invalid";
    }
}


static bool retrieved_all_config(const std::map<std::string, bool> &cfg)
{
    for (const auto &p : cfg)
    {
        if (!p.second)
        {
            return false;
        }
    }
    return true;
}

bool RemoteControl::gather_remote_config(zmqpp::socket &sock, std::list<std::string> &start_list,
        std::list<std::string> &stop_list)
{
    std::map<std::string, bool> cfg;

    // list of name of remote module.
    std::list<std::string> remote_modules;

    if (!gather_remote_module_list(sock, remote_modules))
        return false;

    // we want to retrieve the configuration of the remote modules.
    // so we dont really care about our current module.
    for (const std::string &module_name : remote_modules)
    {
        cfg[module_name] = false;
        zmqpp::message msg;
        // we use boost-text serialization here, since its lighter
        msg << "MODULE_CONFIG" << module_name << ConfigManager::ConfigFormat::BOOST_ARCHIVE;
        INFO("Request remote config for " << module_name);
        sock.send(msg);
    }

    // we want to stop all our module that weren't running on the remote host.
    for (const std::string &my_module : kernel_.module_manager().modules_names())
    {
        stop_list.push_back(my_module);
    }

    // we want to start all remote modules.
    start_list.insert(start_list.end(), remote_modules.begin(), remote_modules.end());

    if (receive_remote_config(sock, cfg))
    {
        INFO("SUCCESS");
        return true;
    }
    return false;
}

bool RemoteControl::gather_remote_module_list(zmqpp::socket &sock, std::list<std::string> &remote_modules)
{
    sock.send("MODULE_LIST");
    zmqpp::poller p;

    p.add(sock);
    p.poll(3000);
    if (p.has_input(sock))
    {
        zmqpp::message msg;
        bool ret = sock.receive(msg);
        assert(ret);
        std::string tmp;
        while (msg.remaining())
        {
            msg >> tmp;
            remote_modules.push_back(tmp);
        }
        return true;
    }
    else
    {
        // no response after 3s.
        WARN("Could'nt get remote module_list in time. Aborting synchronisation");
        return false;
    }
    return false;
}

bool RemoteControl::receive_remote_config(zmqpp::socket &sock, std::map<std::string, bool> &cfg)
{
    zmqpp::poller p;
    p.add(sock);
    while (!retrieved_all_config(cfg))
    {
        zmqpp::message msg;
        std::string ok;
        std::string module;
        std::string binary_cfg;

        p.poll(3000);
        if (p.has_input(sock))
        {
            sock.receive(msg);

            msg >> ok;
            if (ok == "OK")
            {
                assert(msg.parts() >= 3);

                msg >> module >> binary_cfg;
                cfg[module] = true;

                INFO("Updating ConfigManager configuration for module " << module << std::endl
                        << "{" << binary_cfg << "}");
                std::istringstream iss(binary_cfg);
                boost::archive::text_iarchive archive(iss);
                boost::property_tree::ptree cfg_tree;
                boost::property_tree::load(archive, cfg_tree, 1);
                cfg[module] = true;
                kernel_.config_manager().store_config(module, cfg_tree);

                // note this may be dangerous, as it will override ANY file specified here.
                // this is a IMPORTANT security consideration. The remote Leosac can cause damage
                // to the receiving one if its maliciously configured.
                while (msg.remaining())
                {
                    // We consider a pair of frame here:  filepath and content.
                    assert((msg.remaining() % 2) == 0);
                    std::string filepath;
                    std::string content;
                    NOTICE("Writing (or overwriting) config file: " << filepath);
                    msg >> filepath >> content;
                    std::ofstream stream(filepath, std::ofstream::trunc);
                    stream << content;
                }
            }
            else
            {
                ERROR("Error while retrieving config for module.");
                return false;
            }
        }
        else
        {
            ERROR("Receiving remote config timed out");
            return false;
        }
    }
    return true;
}

bool RemoteControl::handle_module_config(zmqpp::message *msg_in, zmqpp::message *msg_out)
{
    assert(msg_in);
    assert(msg_out);

    if (msg_in->remaining() >= 2)
    {
        std::string module_name;
        ConfigManager::ConfigFormat format;
        *msg_in >> module_name >> format;
        module_config(module_name, format, msg_out);
        return true;
    }
    return false;
}

bool RemoteControl::handle_module_list(zmqpp::message *msg_in, zmqpp::message *msg_out)
{
    assert(msg_in);
    assert(msg_out);

    if (msg_in->remaining() == 0)
    {
        module_list(msg_out);
        return true;
    }
        return false;
}

bool RemoteControl::handle_sync_from(zmqpp::message *msg_in, zmqpp::message *msg_out)
{
    assert(msg_in);
    assert(msg_out);

    uint8_t autocommit;

    if (msg_in->remaining() == 3)
    {
        std::string endpoint;
        std::string remote_server_pubkey;

        *msg_in >> endpoint;
        *msg_in >> autocommit;
        *msg_in >> remote_server_pubkey;
        sync_from(endpoint, remote_server_pubkey, msg_out);

        if (autocommit)
        {
            INFO("Saving configuration to disk after synchronization.");
            kernel_.save_config();
        }
        return true;
    }
    return false;
}

bool RemoteControl::handle_save(zmqpp::message *msg_in, zmqpp::message *msg_out)
{
    assert(msg_in);
    assert(msg_out);

    if (msg_in->remaining() == 0)
    {
        if (kernel_.save_config())
            *msg_out << "OK";
        else
            *msg_out << "KO" << "Saving config failed for some unkown reason.";

        return true;
    }
    return false;
}

bool RemoteControl::handle_general_config(zmqpp::message *msg_in, zmqpp::message *msg_out)
{
    assert(msg_in);
    assert(msg_out);

    if (msg_in->remaining() == 1)
    {
        ConfigManager::ConfigFormat format;
        *msg_in >> format;
        general_config(format, msg_out);
        return true;
    }
    return false;
}
