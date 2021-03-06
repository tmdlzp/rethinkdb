// Copyright 2010-2012 RethinkDB, all rights reserved.
#include "arch/address.hpp"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>

#include "errors.hpp"
#include <boost/bind.hpp>

#include "arch/io/network.hpp"
#include "arch/runtime/thread_pool.hpp"

/* Get our hostname as an std::string. */
std::string str_gethostname() {
    const int namelen = _POSIX_HOST_NAME_MAX;

    std::vector<char> bytes(namelen + 1);
    bytes[namelen] = '0';

    int res = gethostname(bytes.data(), namelen);
    guarantee_err(res == 0, "gethostname() failed");
    return std::string(bytes.data());
}

void do_getaddrinfo(const char *node,
                    const char *service,
                    const struct addrinfo *hints,
                    struct addrinfo **res,
                    int *retval,
                    int *errno_res) {
    *errno_res = 0;
    *retval = getaddrinfo(node, service, hints, res);
    if (*retval < 0) {
        *errno_res = errno;
    }
}

/* Format an `in_addr` in dotted deciaml notation. */
template <class addr_t>
std::string ip_to_string(const addr_t &addr, int address_family) {
    char buffer[INET6_ADDRSTRLEN] = { 0 };
    const char *result = inet_ntop(address_family, &addr,
                                   buffer, INET6_ADDRSTRLEN);
    guarantee(result == buffer, "Could not format IP address");
    return std::string(buffer);
}

void hostname_to_ips_internal(const std::string &host,
                              int address_family,
                              std::set<ip_address_t> *ips) {
    struct addrinfo hint;
    memset(&hint, 0, sizeof(hint));
    hint.ai_family = address_family;
    hint.ai_socktype = SOCK_STREAM;

    int res;
    int errno_res;
    struct addrinfo *addrs;
    boost::function<void ()> fn =
        boost::bind(do_getaddrinfo, host.c_str(), static_cast<const char*>(NULL),
                    &hint, &addrs, &res, &errno_res);
    thread_pool_t::run_in_blocker_pool(fn);

    if (res != 0) {
        throw host_lookup_exc_t(host, errno_res);
    }

    guarantee(addrs);
    for (struct addrinfo *ai = addrs; ai; ai = ai->ai_next) {
        ips->insert(ip_address_t(ai->ai_addr));
    }

    freeaddrinfo(addrs);
}

std::set<ip_address_t> hostname_to_ips(const std::string &host) {
    std::set<ip_address_t> ips;

    // Allow an error on one lookup, but not both
    bool errored = false;

    try {
        hostname_to_ips_internal(host, AF_INET, &ips);
    } catch (const host_lookup_exc_t &ex) {
        errored = true;
    }

    try {
        hostname_to_ips_internal(host, AF_INET6, &ips);
    } catch (const host_lookup_exc_t &ex) {
        if (errored) throw;
    }

    return ips;
}

bool check_address_filter(ip_address_t addr, const std::set<ip_address_t> &filter) {
    // The filter is a whitelist, loopback addresses are always whitelisted
    return filter.find(addr) != filter.end() || addr.is_loopback();
}

std::set<ip_address_t> get_local_ips(const std::set<ip_address_t> &filter, bool get_all) {
    std::set<ip_address_t> all_ips;
    std::set<ip_address_t> filtered_ips;

    try {
        all_ips = hostname_to_ips(str_gethostname());
    } catch (const host_lookup_exc_t &ex) {
        // Continue on, this probably means there's no DNS entry for this host
    }

    struct ifaddrs *addrs;
    int res = getifaddrs(&addrs);
    guarantee_err(res == 0, "getifaddrs() failed, could not determine local network interfaces");

    for (struct ifaddrs *current_addr = addrs; current_addr != NULL; current_addr = current_addr->ifa_next) {
        struct sockaddr *addr_data = current_addr->ifa_addr;
        if (addr_data == NULL) {
            continue;
        } else if (addr_data->sa_family == AF_INET || addr_data->sa_family == AF_INET6) {
            all_ips.insert(ip_address_t(addr_data));
        }
    }
    freeifaddrs(addrs);

    // Remove any addresses that don't fit the filter
    for (std::set<ip_address_t>::const_iterator it = all_ips.begin(); it != all_ips.end(); ++it) {
        if (get_all || check_address_filter(*it, filter)) {
            filtered_ips.insert(*it);
        }
    }

    return filtered_ips;
}

addr_type_t sanitize_address_family(int address_family) {
    addr_type_t result;

    switch (address_family) {
    case AF_INET:
        result = RDB_IPV4_ADDR;
        break;
    case AF_INET6:
        result = RDB_IPV6_ADDR;
        break;
    default:
        crash("ip_address_t constructed with unexpected address family: %d", address_family);
    }

    return result;
}

ip_address_t ip_address_t::any(int address_family) {
    ip_address_t result;
    result.addr_type = sanitize_address_family(address_family);

    if (result.is_ipv4()) {
        result.ipv4_addr.s_addr = INADDR_ANY;
    } else if (result.is_ipv6()) {
        result.ipv6_addr = in6addr_any;
    } else {
        throw invalid_address_exc_t("unknown address family");
    }

    return result;
}

ip_address_t::ip_address_t(const sockaddr *sa) {
    addr_type = sanitize_address_family(sa->sa_family);

    if (is_ipv4()) {
        ipv4_addr = reinterpret_cast<const sockaddr_in *>(sa)->sin_addr;
    } else if (is_ipv6()) {
        ipv6_addr = reinterpret_cast<const sockaddr_in6 *>(sa)->sin6_addr;
        ipv6_scope_id = reinterpret_cast<const sockaddr_in6*>(sa)->sin6_scope_id;
    } else {
        throw invalid_address_exc_t("unknown address family");
    }
}

ip_address_t::ip_address_t(const std::string &addr_str) {
    // First attempt to parse as IPv4, then try IPv6
    if (inet_pton(AF_INET, addr_str.c_str(), &ipv4_addr) == 1) {
        addr_type = RDB_IPV4_ADDR;
    } else if (inet_pton(AF_INET6, addr_str.c_str(), &ipv6_addr) == 1) {
        addr_type = RDB_IPV6_ADDR;
    } else {
        throw invalid_address_exc_t(strprintf("could not parse IP address from string: '%s'", addr_str.c_str()));
    }
}

int ip_address_t::get_address_family() const {
    int result;

    switch (addr_type) {
    case RDB_UNSPEC_ADDR:
        result = AF_UNSPEC;
        break;
    case RDB_IPV4_ADDR:
        result = AF_INET;
        break;
    case RDB_IPV6_ADDR:
        result = AF_INET6;
        break;
    default :
        crash("unknown ip_address_t address type: %d", addr_type);
    }

    return result;
}

const struct in_addr &ip_address_t::get_ipv4_addr() const {
    if (!is_ipv4()) {
        throw invalid_address_exc_t("get_ipv4_addr() called on a non-IPv4 ip_address_t");
    }
    return ipv4_addr;
}

const struct in6_addr &ip_address_t::get_ipv6_addr() const {
    if (!is_ipv6()) {
        throw invalid_address_exc_t("get_ipv6_addr() called on a non-IPv6 ip_address_t");
    }
    return ipv6_addr;
}

uint32_t ip_address_t::get_ipv6_scope_id() const {
    if (!is_ipv6()) {
        throw invalid_address_exc_t("get_ipv6_scope_id() called on a non-IPv6 ip_address_t");
    }
    return ipv6_scope_id;
}

std::string ip_address_t::to_string() const {
    std::string result;

    if (is_ipv4()) {
        result = ip_to_string(ipv4_addr, AF_INET);
    } else if (is_ipv6()) {
        result = ip_to_string(ipv6_addr, AF_INET6);
    } else {
        crash("to_string called on an uninitialized ip_address_t, addr_type: %d", addr_type);
    }

    return result;
}

bool ip_address_t::operator == (const ip_address_t &x) const {
    if (addr_type == x.addr_type) {
        if (is_ipv4()) {
            return ipv4_addr.s_addr == x.ipv4_addr.s_addr;
        } else if (is_ipv6()) {
            return IN6_ARE_ADDR_EQUAL(&ipv6_addr, &x.ipv6_addr);
        }
        return true;
    }
    return false;
}

bool ip_address_t::operator < (const ip_address_t &x) const {
    if (addr_type == x.addr_type) {
        if (is_ipv4()) {
            return ipv4_addr.s_addr < x.ipv4_addr.s_addr;
        } else if (is_ipv6()) {
            return memcmp(&ipv6_addr, &x.ipv6_addr, sizeof(ipv6_addr)) < 0;
        } else {
            return false;
        }
    }
    return addr_type < x.addr_type;
}

bool ip_address_t::is_loopback() const {
    if (is_ipv4()) {
        return (ntohl(ipv4_addr.s_addr) >> 24) == IN_LOOPBACKNET;
    } else if (is_ipv6()) {
        return IN6_IS_ADDR_LOOPBACK(&ipv6_addr);
    }
    return false;
}

bool ip_address_t::is_any() const {
    if (is_ipv4()) {
        return ipv4_addr.s_addr == INADDR_ANY;
    } else if (is_ipv6()) {
        return IN6_IS_ADDR_UNSPECIFIED(&ipv6_addr);
    }
    return false;
}

port_t::port_t(int _value) : value_(_value) {
    guarantee(value_ <= MAX_PORT);
}

int port_t::value() const {
    return value_;
}

ip_and_port_t::ip_and_port_t() :
    port_(0)
{ }

ip_and_port_t::ip_and_port_t(const ip_address_t &_ip, port_t _port) :
    ip_(_ip), port_(_port)
{ }

bool ip_and_port_t::operator < (const ip_and_port_t &other) const {
    if (ip_ == other.ip_) {
        return port_.value() < other.port_.value();
    }
    return ip_ < other.ip_;
}

bool ip_and_port_t::operator == (const ip_and_port_t &other) const {
    return ip_ == other.ip_ && port_.value() == other.port_.value();
}

const ip_address_t & ip_and_port_t::ip() const {
    return ip_;
}

port_t ip_and_port_t::port() const {
    return port_;
}

host_and_port_t::host_and_port_t() :
    port_(0)
{ }

host_and_port_t::host_and_port_t(const std::string& _host, port_t _port) :
    host_(_host), port_(_port)
{ }

bool host_and_port_t::operator < (const host_and_port_t &other) const {
    if (host_ == other.host_) {
        return port_.value() < other.port_.value();
    }
    return host_ < other.host_;
}

bool host_and_port_t::operator == (const host_and_port_t &other) const {
    return host_ == other.host_ && port_.value() == other.port_.value();
}

std::set<ip_and_port_t> host_and_port_t::resolve() const {
    std::set<ip_and_port_t> result;
    std::set<ip_address_t> host_ips = hostname_to_ips(host_);
    for (auto jt = host_ips.begin(); jt != host_ips.end(); ++jt) {
        result.insert(ip_and_port_t(*jt, port_));
    }
    return result;
}

const std::string & host_and_port_t::host() const {
    return host_;
}

port_t host_and_port_t::port() const {
    return port_;
}

peer_address_t::peer_address_t(const std::set<host_and_port_t> &_hosts) :
    hosts_(_hosts)
{
    for (auto it = hosts_.begin(); it != hosts_.end(); ++it) {
        std::set<ip_and_port_t> host_ips = it->resolve();
        resolved_ips.insert(host_ips.begin(), host_ips.end());
    }
}

peer_address_t::peer_address_t() { }

const std::set<host_and_port_t>& peer_address_t::hosts() const {
    return hosts_;
}

host_and_port_t peer_address_t::primary_host() const {
    guarantee(!hosts_.empty());
    return *hosts_.begin();
}

const std::set<ip_and_port_t>& peer_address_t::ips() const {
    return resolved_ips;
}

// Two addresses are considered equal if all of their hosts match
bool peer_address_t::operator == (const peer_address_t &a) const {
    std::set<host_and_port_t>::const_iterator it, jt;
    for (it = hosts_.begin(), jt = a.hosts_.begin();
         it != hosts_.end() && jt != a.hosts_.end(); ++it, ++jt) {
        if (it->port().value() != jt->port().value() ||
            it->host() != jt->host()) {
            return false;
        }
    }
    return true;
}

bool peer_address_t::operator != (const peer_address_t &a) const {
    return !(*this == a);
}

void debug_print(printf_buffer_t *buf, const ip_address_t &addr) {
    buf->appendf("%s", addr.to_string().c_str());
}

void debug_print(printf_buffer_t *buf, const ip_and_port_t &addr) {
    buf->appendf("%s:%d", addr.ip().to_string().c_str(), addr.port().value());
}

void debug_print(printf_buffer_t *buf, const host_and_port_t &addr) {
    buf->appendf("%s:%d", addr.host().c_str(), addr.port().value());
}

void debug_print(printf_buffer_t *buf, const peer_address_t &address) {
    buf->appendf("peer_address [");
    const std::set<host_and_port_t> &hosts = address.hosts();
    for (auto it = hosts.begin(); it != hosts.end(); ++it) {
        if (it != hosts.begin()) buf->appendf(", ");
        debug_print(buf, *it);
    }
    buf->appendf("]");
}
