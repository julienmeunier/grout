// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2024 Robin Jarry

#include "worker_priv.h"

#include <br_api.h>
#include <br_cmocka.h>
#include <br_control.h>
#include <br_infra.h>
#include <br_port.h>
#include <br_queue.h>
#include <br_stb_ds.h>
#include <br_worker.h>

#include <numa.h>
#include <rte_ethdev.h>

static struct port p0 = {.port_id = 0, .n_rxq = 2};
static struct port p1 = {.port_id = 1, .n_rxq = 2};
static struct port p2 = {.port_id = 2, .n_rxq = 2};
static struct worker w1 = {.cpu_id = 1, .started = true};
static struct worker w2 = {.cpu_id = 2, .started = true};
static struct worker w3 = {.cpu_id = 3, .started = true};
static struct rte_eth_dev_info dev_info = {.nb_rx_queues = 2};

// mocked types/functions
int br_rte_log_type;
void br_register_api_handler(struct br_api_handler *) { }
void br_register_module(struct br_module *) { }

mock_func(int, worker_graph_reload_all(void));
mock_func(void, worker_graph_free(struct worker *));
mock_func(void *, br_datapath_loop(void *));
mock_func(void, __wrap_rte_free(void *));
mock_func(int, __wrap_rte_eth_dev_stop(uint16_t));
mock_func(
	struct rte_mempool *,
	__wrap_rte_pktmbuf_pool_create(
		const char *,
		unsigned int,
		unsigned int,
		uint16_t,
		uint16_t,
		int
	)
);
mock_func(void, __wrap_rte_mempool_free(struct rte_mempool *));
mock_func(
	int,
	__wrap_rte_eth_dev_info_get(uint16_t, struct rte_eth_dev_info *info),
	memcpy(info, &dev_info, sizeof(*info))
);
mock_func(
	int,
	__wrap_rte_eth_rx_queue_setup(uint16_t, uint16_t, uint16_t, unsigned int, const struct rte_eth_rxconf *, struct rte_mempool *)
);
mock_func(
	int,
	__wrap_rte_eth_tx_queue_setup(uint16_t, uint16_t, uint16_t, unsigned int, const struct rte_eth_txconf *)
);
mock_func(int, __wrap_numa_node_of_cpu(int));
mock_func(int, __wrap_rte_eth_dev_start(uint16_t));
mock_func(const char *, __wrap_rte_dev_name(struct rte_device *));
mock_func(
	int,
	__wrap_rte_eth_dev_configure(uint16_t, uint16_t, uint16_t, const struct rte_eth_conf *)
);
mock_func(int, __wrap_pthread_create(pthread_t *, const pthread_attr_t *, void *(void *), void *));
mock_func(int, __wrap_pthread_join(pthread_t *, const pthread_attr_t *, void *(void *), void *));
mock_func(void *, __wrap_rte_zmalloc(char *, size_t, unsigned));
mock_func(unsigned, __wrap_rte_get_main_lcore(void));
mock_func(int, __wrap_numa_bitmask_isbitset(const struct bitmask *, int));

#define assert_qmaps(qmaps, ...)                                                                   \
	do {                                                                                       \
		struct queue_map __expected[] = {__VA_ARGS__};                                     \
		int __len = sizeof(__expected) / sizeof(struct queue_map);                         \
		if (arrlen(qmaps) != __len)                                                        \
			fail_msg("%s len %li expected %i", #qmaps, arrlen(qmaps), __len);          \
		for (int __i = 0; __i < __len; __i++) {                                            \
			struct queue_map *exp = &__expected[__i], *act;                            \
			bool found = false;                                                        \
			arrforeach (act, qmaps) {                                                  \
				if (act->port_id != exp->port_id)                                  \
					continue;                                                  \
				if (act->queue_id != exp->queue_id)                                \
					continue;                                                  \
				found = true;                                                      \
				break;                                                             \
			}                                                                          \
			if (!found)                                                                \
				fail_msg(                                                          \
					"%s port %u queue %u not found",                           \
					#qmaps,                                                    \
					exp->port_id,                                              \
					exp->queue_id                                              \
				);                                                                 \
		}                                                                                  \
	} while (0)

static struct queue_map q(uint16_t port_id, uint16_t rxq_id) {
	struct queue_map qmap = {
		.port_id = port_id,
		.queue_id = rxq_id,
		.enabled = true,
	};
	return qmap;
}

static int setup(void **) {
	LIST_INSERT_HEAD(&ports, &p0, next);
	LIST_INSERT_HEAD(&ports, &p1, next);
	LIST_INSERT_HEAD(&ports, &p2, next);

	LIST_INSERT_HEAD(&workers, &w1, next);
	arrpush(w1.rxqs, q(0, 0));
	arrpush(w1.rxqs, q(0, 1));
	arrpush(w1.rxqs, q(1, 0));

	arrpush(w1.txqs, q(0, 0));
	arrpush(w1.txqs, q(1, 0));
	arrpush(w1.txqs, q(2, 0));

	LIST_INSERT_HEAD(&workers, &w2, next);
	arrpush(w2.rxqs, q(1, 1));
	arrpush(w2.rxqs, q(2, 0));
	arrpush(w2.rxqs, q(2, 1));

	arrpush(w2.txqs, q(0, 1));
	arrpush(w2.txqs, q(1, 1));
	arrpush(w2.txqs, q(2, 1));

	return 0;
}

static int teardown(void **) {
	struct worker *w;
	LIST_FOREACH (w, &workers, next) {
		arrfree(w->rxqs);
		arrfree(w->txqs);
	}
	return 0;
}

static void common_mocks(void) {
	will_return_maybe(worker_graph_free, 0);
	will_return_maybe(worker_graph_reload_all, 0);
	will_return_maybe(__wrap_numa_bitmask_isbitset, 1);
	will_return_maybe(__wrap_pthread_create, 0);
	will_return_maybe(__wrap_pthread_join, 0);
	will_return_maybe(__wrap_rte_dev_name, "");
	will_return_maybe(__wrap_rte_eth_dev_configure, 0);
	will_return_maybe(__wrap_rte_eth_dev_info_get, 0);
	will_return_maybe(__wrap_rte_eth_dev_start, 0);
	will_return_maybe(__wrap_rte_eth_dev_stop, 0);
	will_return_maybe(__wrap_rte_eth_rx_queue_setup, 0);
	will_return_maybe(__wrap_rte_eth_tx_queue_setup, 0);
	will_return_maybe(__wrap_rte_free, 0);
	will_return_maybe(__wrap_rte_get_main_lcore, 0);
	will_return_maybe(__wrap_rte_mempool_free, 0);
	will_return_maybe(__wrap_rte_pktmbuf_pool_create, 1);
}

static void rxq_assign_main_lcore(void **) {
	will_return(__wrap_rte_get_main_lcore, 4);
	assert_int_equal(worker_rxq_assign(0, 0, 4), -EBUSY);
}

static void rxq_assign_invalid_cpu(void **) {
	will_return(__wrap_rte_get_main_lcore, 0);
	will_return(__wrap_numa_bitmask_isbitset, 0);
	assert_int_equal(worker_rxq_assign(0, 0, 9999), -ERANGE);
}

static void rxq_assign_invalid_port(void **) {
	common_mocks();
	assert_int_equal(worker_rxq_assign(9999, 0, 1), -ENODEV);
}

static void rxq_assign_invalid_rxq(void **) {
	common_mocks();
	assert_int_equal(worker_rxq_assign(0, 9999, 1), -ENODEV);
}

static void rxq_assign_already_set(void **) {
	common_mocks();
	assert_int_equal(worker_rxq_assign(1, 1, 2), 0);
}

static void rxq_assign_existing_worker(void **) {
	common_mocks();
	assert_int_equal(worker_rxq_assign(1, 1, 1), 0);
	assert_int_equal(worker_count(), 2);
	assert_qmaps(w1.rxqs, q(0, 0), q(0, 1), q(1, 0), q(1, 1));
	assert_qmaps(w2.rxqs, q(2, 1), q(2, 0));
	assert_qmaps(w3.rxqs);
	assert_qmaps(w1.txqs, q(0, 0), q(1, 0), q(2, 0));
	assert_qmaps(w2.txqs, q(0, 1), q(1, 1), q(2, 1));
	assert_qmaps(w3.txqs);
}

static void rxq_assign_existing_worker_destroy(void **) {
	common_mocks();

	assert_int_equal(worker_rxq_assign(2, 0, 1), 0);
	assert_int_equal(worker_count(), 2);
	assert_qmaps(w1.rxqs, q(0, 0), q(0, 1), q(1, 0), q(1, 1), q(2, 0));
	assert_qmaps(w2.rxqs, q(2, 1));
	assert_qmaps(w3.rxqs);
	assert_qmaps(w1.txqs, q(0, 0), q(1, 0), q(2, 0));
	assert_qmaps(w2.txqs, q(0, 1), q(1, 1), q(2, 1));
	assert_qmaps(w3.txqs);

	assert_int_equal(worker_rxq_assign(2, 1, 1), 0);
	assert_int_equal(worker_count(), 1);
	assert_qmaps(w1.rxqs, q(0, 0), q(0, 1), q(1, 0), q(1, 1), q(2, 0), q(2, 1));
	assert_qmaps(w2.rxqs);
	assert_qmaps(w3.rxqs);
	assert_qmaps(w1.txqs, q(1, 0), q(2, 0), q(0, 0));
	assert_qmaps(w2.txqs);
	assert_qmaps(w3.txqs);
}

static void rxq_assign_new_worker(void **) {
	common_mocks();

	will_return(__wrap_rte_zmalloc, &w2);
	assert_int_equal(worker_rxq_assign(2, 1, 2), 0);
	assert_int_equal(worker_count(), 2);
	assert_qmaps(w1.rxqs, q(0, 0), q(0, 1), q(1, 0), q(1, 1), q(2, 0));
	assert_qmaps(w2.rxqs, q(2, 1));
	assert_qmaps(w3.rxqs);
	assert_qmaps(w1.txqs, q(0, 1), q(1, 1), q(2, 1));
	assert_qmaps(w2.txqs, q(0, 0), q(1, 0), q(2, 0));
	assert_qmaps(w3.txqs);
}

static void rxq_assign_new_worker_destroy(void **) {
	common_mocks();

	will_return(__wrap_rte_zmalloc, &w3);
	assert_int_equal(worker_rxq_assign(2, 1, 3), 0);
	assert_int_equal(worker_count(), 2);
	assert_qmaps(w1.rxqs, q(0, 0), q(0, 1), q(1, 0), q(1, 1), q(2, 0));
	assert_qmaps(w2.rxqs);
	assert_qmaps(w3.rxqs, q(2, 1));
	assert_qmaps(w1.txqs, q(0, 1), q(1, 1), q(2, 1));
	assert_qmaps(w2.txqs);
	assert_qmaps(w3.txqs, q(0, 0), q(1, 0), q(2, 0));
}

static void rxq_assign_new_worker2(void **) {
	common_mocks();

	will_return(__wrap_rte_zmalloc, &w2);
	assert_int_equal(worker_rxq_assign(2, 0, 2), 0);
	assert_int_equal(worker_count(), 3);
	assert_qmaps(w1.rxqs, q(0, 0), q(0, 1), q(1, 0), q(1, 1));
	assert_qmaps(w2.rxqs, q(2, 0));
	assert_qmaps(w3.rxqs, q(2, 1));
	assert_qmaps(w1.txqs, q(0, 2), q(1, 2), q(2, 2));
	assert_qmaps(w2.txqs, q(0, 0), q(1, 0), q(2, 0));
	assert_qmaps(w3.txqs, q(0, 1), q(1, 1), q(2, 1));
}

int main(void) {
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(rxq_assign_main_lcore),
		cmocka_unit_test(rxq_assign_invalid_cpu),
		cmocka_unit_test(rxq_assign_invalid_port),
		cmocka_unit_test(rxq_assign_invalid_rxq),
		cmocka_unit_test(rxq_assign_already_set),
		cmocka_unit_test(rxq_assign_existing_worker),
		cmocka_unit_test(rxq_assign_existing_worker_destroy),
		cmocka_unit_test(rxq_assign_new_worker),
		cmocka_unit_test(rxq_assign_new_worker_destroy),
		cmocka_unit_test(rxq_assign_new_worker2),
	};
	return cmocka_run_group_tests(tests, setup, teardown);
}