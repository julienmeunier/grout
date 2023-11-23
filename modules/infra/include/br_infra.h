// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2023 Robin Jarry

#ifndef _BR_INFRA
#define _BR_INFRA

#include <br_api.h>

#include <rte_ether.h>

#define BR_INFRA_MODULE 0xacdc

struct br_infra_port {
	uint16_t index;
	char name[64];
	char device[128];
	uint16_t mtu;
	struct rte_ether_addr mac;
};

#define BR_INFRA_PORT_ADD REQUEST_TYPE(BR_INFRA_MODULE, 1)

struct br_infra_port_add_req {
	char name[64];
	char devargs[128];
};

struct br_infra_port_add_resp {
	struct br_infra_port port;
};

#define BR_INFRA_PORT_DEL REQUEST_TYPE(BR_INFRA_MODULE, 3)

struct br_infra_port_del_req {
	char name[64];
};

// struct br_infra_port_del_resp { };

#define BR_INFRA_PORT_GET REQUEST_TYPE(BR_INFRA_MODULE, 2)

struct br_infra_port_get_req {
	char name[64];
};

struct br_infra_port_get_resp {
	struct br_infra_port port;
};

#define BR_INFRA_PORT_LIST REQUEST_TYPE(BR_INFRA_MODULE, 4)

// struct br_infra_port_list_req { };

struct br_infra_port_list_resp {
	uint16_t n_ports;
	struct br_infra_port ports[32];
};

#endif
