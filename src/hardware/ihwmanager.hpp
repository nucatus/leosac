/**
 * \file ihwmanager.hpp
 * \author Thibault Schueller <ryp.sqrt@gmail.com>
 * \brief hardware managing class interface
 */

#ifndef IHWMANAGER_HPP
#define IHWMANAGER_HPP

#include <string>

class IDevice;

class IHWManager
{
public:
    typedef struct {
        std::string name;
    } PlatformInfo;

public:
    virtual ~IHWManager() {}
    virtual void                start() = 0;
    virtual void                stop() = 0;
    virtual IDevice*            getDevice(const std::string& name) = 0;
    virtual const PlatformInfo& getPlatformInfo() const = 0;
};

#endif // IHWMANAGER_HPP