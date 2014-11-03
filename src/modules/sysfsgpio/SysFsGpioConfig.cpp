#include <tools/log.hpp>
#include "SysFsGpioConfig.hpp"
#include <boost/algorithm/string.hpp>

using namespace Leosac::Module::SysFsGpio;

SysFsGpioConfig::SysFsGpioConfig(const boost::property_tree::ptree &cfg)
{
    cfg_export_path_ = cfg.get<std::string>("export_path");
    cfg_unexport_path_ = cfg.get<std::string>("unexport_path");
    cfg_value_path_ = cfg.get<std::string>("value_path");
    cfg_edge_path_ = cfg.get<std::string>("edge_path");
    cfg_direction_path_ = cfg.get<std::string>("direction_path");

    auto aliases_cfg = cfg.get_child("aliases");
    for (auto alias : aliases_cfg)
    {
        if (alias.first == "default")
        {
            default_aliases_ = alias.second.data();
            continue;
        }

        int pin_no = std::stoi(alias.first);
        pin_aliases_[pin_no] = alias.second.data();
    }

    INFO("SysFsGpio Path Configuration:" << std::endl
            << '\t' << "Export path: " << cfg_export_path_ << std::endl
            << '\t' << "Unexport path: " << cfg_unexport_path_ << std::endl
            << '\t' << "Value path: " << cfg_value_path_ << std::endl
            << '\t' << "Edge path: " << cfg_edge_path_ << std::endl
            << '\t' << "Direction path: " << cfg_direction_path_ << std::endl
            << '\t' << "Default aliases rule: " << default_aliases_ << std::endl);
}


const std::string &SysFsGpioConfig::export_path() const
{
    return cfg_export_path_;
}

const std::string &SysFsGpioConfig::unexport_path() const
{
    return cfg_unexport_path_;
}

std::string SysFsGpioConfig::value_path(int pin_no) const
{
    if (pin_aliases_.count(pin_no))
        return boost::replace_all_copy(cfg_value_path_, "__REPLACE_ME__", pin_aliases_.find(pin_no)->second);
    return boost::replace_all_copy(cfg_value_path_, "__REPLACE_ME__", boost::replace_all_copy(default_aliases_, "__NO__", std::to_string(pin_no)));
}

std::string SysFsGpioConfig::edge_path(int pin_no) const
{
     if (pin_aliases_.count(pin_no))
        return boost::replace_all_copy(cfg_edge_path_, "__REPLACE_ME__", pin_aliases_.find(pin_no)->second);
    return boost::replace_all_copy(cfg_edge_path_, "__REPLACE_ME__", boost::replace_all_copy(default_aliases_, "__NO__", std::to_string(pin_no)));
}

std::string SysFsGpioConfig::direction_path(int pin_no) const
{
    if (pin_aliases_.count(pin_no))
        return boost::replace_all_copy(cfg_direction_path_, "__REPLACE_ME__", pin_aliases_.find(pin_no)->second);
    return boost::replace_all_copy(cfg_direction_path_, "__REPLACE_ME__", boost::replace_all_copy(default_aliases_, "__NO__", std::to_string(pin_no)));
}