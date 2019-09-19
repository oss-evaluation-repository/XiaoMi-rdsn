// Copyright (c) 2017-present, Xiaomi, Inc.  All rights reserved.
// This source code is licensed under the Apache License Version 2.0, which
// can be found in the LICENSE file in the root directory of this source tree.

#include <string>

#include <dsn/c/api_layer1.h>
#include <dsn/cpp/serialization_helper/dsn.layer2_types.h>
#include <dsn/dist/replication/replication_types.h>
#include <dsn/utility/config_api.h>
#include <dsn/utility/output_utils.h>

#include "server_load_balancer.h"
#include "server_state.h"
#include "meta_http_service.h"
#include "meta_server_failure_detector.h"

namespace dsn {
namespace replication {

struct list_nodes_helper
{
    std::string node_address;
    std::string node_status;
    int primary_count;
    int secondary_count;
    list_nodes_helper(const std::string &a, const std::string &s)
        : node_address(a), node_status(s), primary_count(0), secondary_count(0)
    {
    }
};

void meta_http_service::get_app_handler(const http_request &req, http_response &resp)
{
    std::string app_name;
    bool detailed = false;
    for (const auto &p : req.query_args) {
        if (p.first == "name") {
            app_name = p.second;
        } else if (p.first == "detail") {
            detailed = true;
        } else {
            resp.status_code = http_status_code::bad_request;
            return;
        }
    }
    if (!redirect_if_not_primary(req, resp))
        return;

    configuration_query_by_index_request request;
    configuration_query_by_index_response response;

    request.app_name = app_name;
    _service->_state->query_configuration_by_index(request, response);
    if (response.err == ERR_OBJECT_NOT_FOUND) {
        resp.status_code = http_status_code::not_found;
        resp.body = fmt::format("table not found: \"{}\"", app_name);
        return;
    }
    if (response.err != dsn::ERR_OK) {
        resp.body = response.err.to_string();
        resp.status_code = http_status_code::internal_server_error;
        return;
    }

    // output as json format
    dsn::utils::multi_table_printer mtp;
    std::ostringstream out;
    dsn::utils::table_printer tp_general("general");
    tp_general.add_row_name_and_data("app_name", app_name);
    tp_general.add_row_name_and_data("app_id", response.app_id);
    tp_general.add_row_name_and_data("partition_count", response.partition_count);
    if (!response.partitions.empty()) {
        tp_general.add_row_name_and_data("max_replica_count",
                                         response.partitions[0].max_replica_count);
    } else {
        tp_general.add_row_name_and_data("max_replica_count", 0);
    }
    mtp.add(std::move(tp_general));

    if (detailed) {
        dsn::utils::table_printer tp_details("replicas");
        tp_details.add_title("pidx");
        tp_details.add_column("ballot");
        tp_details.add_column("replica_count");
        tp_details.add_column("primary");
        tp_details.add_column("secondaries");
        std::map<rpc_address, std::pair<int, int>> node_stat;

        int total_prim_count = 0;
        int total_sec_count = 0;
        int fully_healthy = 0;
        int write_unhealthy = 0;
        int read_unhealthy = 0;
        for (const auto &p : response.partitions) {
            int replica_count = 0;
            if (!p.primary.is_invalid()) {
                replica_count++;
                node_stat[p.primary].first++;
                total_prim_count++;
            }
            replica_count += p.secondaries.size();
            total_sec_count += p.secondaries.size();
            if (!p.primary.is_invalid()) {
                if (replica_count >= p.max_replica_count)
                    fully_healthy++;
                else if (replica_count < 2)
                    write_unhealthy++;
            } else {
                write_unhealthy++;
                read_unhealthy++;
            }
            tp_details.add_row(p.pid.get_partition_index());
            tp_details.append_data(p.ballot);
            std::stringstream oss;
            oss << replica_count << "/" << p.max_replica_count;
            tp_details.append_data(oss.str());
            tp_details.append_data((p.primary.is_invalid() ? "-" : p.primary.to_std_string()));
            oss.str("");
            oss << "[";
            for (int j = 0; j < p.secondaries.size(); j++) {
                if (j != 0)
                    oss << ",";
                oss << p.secondaries[j].to_std_string();
                node_stat[p.secondaries[j]].second++;
            }
            oss << "]";
            tp_details.append_data(oss.str());
        }
        mtp.add(std::move(tp_details));

        // 'node' section.
        dsn::utils::table_printer tp_nodes("nodes");
        tp_nodes.add_title("node");
        tp_nodes.add_column("primary");
        tp_nodes.add_column("secondary");
        tp_nodes.add_column("total");
        for (auto &kv : node_stat) {
            tp_nodes.add_row(kv.first.to_std_string());
            tp_nodes.append_data(kv.second.first);
            tp_nodes.append_data(kv.second.second);
            tp_nodes.append_data(kv.second.first + kv.second.second);
        }
        tp_nodes.add_row("total");
        tp_nodes.append_data(total_prim_count);
        tp_nodes.append_data(total_sec_count);
        tp_nodes.append_data(total_prim_count + total_sec_count);
        mtp.add(std::move(tp_nodes));

        // healthy partition count section.
        dsn::utils::table_printer tp_hpc("healthy");
        tp_hpc.add_row_name_and_data("fully_healthy_partition_count", fully_healthy);
        tp_hpc.add_row_name_and_data("unhealthy_partition_count",
                                     response.partition_count - fully_healthy);
        tp_hpc.add_row_name_and_data("write_unhealthy_partition_count", write_unhealthy);
        tp_hpc.add_row_name_and_data("read_unhealthy_partition_count", read_unhealthy);
        mtp.add(std::move(tp_hpc));
    }

    mtp.output(out, dsn::utils::table_printer::output_format::kJsonCompact);
    resp.body = out.str();
    resp.status_code = http_status_code::ok;
}

void meta_http_service::list_app_handler(const http_request &req, http_response &resp)
{
    bool detailed = false;
    for (const auto &p : req.query_args) {
        if (p.first == "detail") {
            detailed = true;
        } else {
            resp.status_code = http_status_code::bad_request;
            return;
        }
    }
    if (!redirect_if_not_primary(req, resp))
        return;
    configuration_list_apps_response response;
    configuration_list_apps_request request;
    request.status = dsn::app_status::AS_INVALID;

    _service->_state->list_apps(request, response);

    if (response.err != dsn::ERR_OK) {
        resp.body = response.err.to_string();
        resp.status_code = http_status_code::internal_server_error;
        return;
    }
    std::vector<::dsn::app_info> &apps = response.infos;

    // output as json format
    std::ostringstream out;
    dsn::utils::multi_table_printer mtp;
    int available_app_count = 0;
    dsn::utils::table_printer tp_general("general_info");
    tp_general.add_title("app_id");
    tp_general.add_column("status");
    tp_general.add_column("app_name");
    tp_general.add_column("app_type");
    tp_general.add_column("partition_count");
    tp_general.add_column("replica_count");
    tp_general.add_column("is_stateful");
    tp_general.add_column("create_time");
    tp_general.add_column("drop_time");
    tp_general.add_column("drop_expire");
    tp_general.add_column("envs_count");
    for (const auto &app : apps) {
        if (app.status != dsn::app_status::AS_AVAILABLE) {
            continue;
        }
        std::string status_str = enum_to_string(app.status);
        status_str = status_str.substr(status_str.find("AS_") + 3);
        std::string create_time = "-";
        if (app.create_second > 0) {
            char buf[24];
            dsn::utils::time_ms_to_string((uint64_t)app.create_second * 1000, buf);
            create_time = buf;
        }
        std::string drop_time = "-";
        std::string drop_expire_time = "-";
        if (app.status == app_status::AS_AVAILABLE) {
            available_app_count++;
        } else if (app.status == app_status::AS_DROPPED && app.expire_second > 0) {
            if (app.drop_second > 0) {
                char buf[24];
                dsn::utils::time_ms_to_string((uint64_t)app.drop_second * 1000, buf);
                drop_time = buf;
            }
            if (app.expire_second > 0) {
                char buf[24];
                dsn::utils::time_ms_to_string((uint64_t)app.expire_second * 1000, buf);
                drop_expire_time = buf;
            }
        }

        tp_general.add_row(app.app_id);
        tp_general.append_data(status_str);
        tp_general.append_data(app.app_name);
        tp_general.append_data(app.app_type);
        tp_general.append_data(app.partition_count);
        tp_general.append_data(app.max_replica_count);
        tp_general.append_data(app.is_stateful);
        tp_general.append_data(create_time);
        tp_general.append_data(drop_time);
        tp_general.append_data(drop_expire_time);
        tp_general.append_data(app.envs.size());
    }
    mtp.add(std::move(tp_general));

    int total_fully_healthy_app_count = 0;
    int total_unhealthy_app_count = 0;
    int total_write_unhealthy_app_count = 0;
    int total_read_unhealthy_app_count = 0;
    if (detailed && available_app_count > 0) {
        dsn::utils::table_printer tp_health("healthy_info");
        tp_health.add_title("app_id");
        tp_health.add_column("app_name");
        tp_health.add_column("partition_count");
        tp_health.add_column("fully_healthy");
        tp_health.add_column("unhealthy");
        tp_health.add_column("write_unhealthy");
        tp_health.add_column("read_unhealthy");
        for (auto &info : apps) {
            if (info.status != app_status::AS_AVAILABLE) {
                continue;
            }
            configuration_query_by_index_request request;
            configuration_query_by_index_response response;
            request.app_name = info.app_name;
            _service->_state->query_configuration_by_index(request, response);
            dassert(info.app_id == response.app_id,
                    "invalid app_id, %d VS %d",
                    info.app_id,
                    response.app_id);
            dassert(info.partition_count == response.partition_count,
                    "invalid partition_count, %d VS %d",
                    info.partition_count,
                    response.partition_count);
            int fully_healthy = 0;
            int write_unhealthy = 0;
            int read_unhealthy = 0;
            for (int i = 0; i < response.partitions.size(); i++) {
                const dsn::partition_configuration &p = response.partitions[i];
                int replica_count = 0;
                if (!p.primary.is_invalid()) {
                    replica_count++;
                }
                replica_count += p.secondaries.size();
                if (!p.primary.is_invalid()) {
                    if (replica_count >= p.max_replica_count)
                        fully_healthy++;
                    else if (replica_count < 2)
                        write_unhealthy++;
                } else {
                    write_unhealthy++;
                    read_unhealthy++;
                }
            }
            tp_health.add_row(info.app_id);
            tp_health.append_data(info.app_name);
            tp_health.append_data(info.partition_count);
            tp_health.append_data(fully_healthy);
            tp_health.append_data(info.partition_count - fully_healthy);
            tp_health.append_data(write_unhealthy);
            tp_health.append_data(read_unhealthy);

            if (fully_healthy == info.partition_count)
                total_fully_healthy_app_count++;
            else
                total_unhealthy_app_count++;
            if (write_unhealthy > 0)
                total_write_unhealthy_app_count++;
            if (read_unhealthy > 0)
                total_read_unhealthy_app_count++;
        }
        mtp.add(std::move(tp_health));
    }

    dsn::utils::table_printer tp_count("summary");
    tp_count.add_row_name_and_data("total_app_count", available_app_count);
    if (detailed && available_app_count > 0) {
        tp_count.add_row_name_and_data("fully_healthy_app_count", total_fully_healthy_app_count);
        tp_count.add_row_name_and_data("unhealthy_app_count", total_unhealthy_app_count);
        tp_count.add_row_name_and_data("write_unhealthy_app_count",
                                       total_write_unhealthy_app_count);
        tp_count.add_row_name_and_data("read_unhealthy_app_count", total_read_unhealthy_app_count);
    }
    mtp.add(std::move(tp_count));

    mtp.output(out, dsn::utils::table_printer::output_format::kJsonCompact);

    resp.body = out.str();
    resp.status_code = http_status_code::ok;
}

void meta_http_service::list_node_handler(const http_request &req, http_response &resp)
{
    bool detailed = false;
    for (const auto &p : req.query_args) {
        if (p.first == "detail") {
            detailed = true;
        } else {
            resp.status_code = http_status_code::bad_request;
            return;
        }
    }
    if (!redirect_if_not_primary(req, resp))
        return;

    std::map<dsn::rpc_address, list_nodes_helper> tmp_map;
    for (const auto &node : _service->_alive_set) {
        tmp_map.emplace(node, list_nodes_helper(node.to_std_string(), "ALIVE"));
    }
    for (const auto &node : _service->_dead_set) {
        tmp_map.emplace(node, list_nodes_helper(node.to_std_string(), "UNALIVE"));
    }
    int alive_node_count = (_service->_alive_set).size();
    int unalive_node_count = (_service->_dead_set).size();

    if (detailed) {
        configuration_list_apps_response response;
        configuration_list_apps_request request;
        request.status = dsn::app_status::AS_AVAILABLE;
        _service->_state->list_apps(request, response);
        for (const auto &app : response.infos) {
            configuration_query_by_index_request request_app;
            configuration_query_by_index_response response_app;
            request_app.app_name = app.app_name;
            _service->_state->query_configuration_by_index(request_app, response_app);
            dassert(app.app_id == response_app.app_id,
                    "invalid app_id, %d VS %d",
                    app.app_id,
                    response_app.app_id);
            dassert(app.partition_count == response_app.partition_count,
                    "invalid partition_count, %d VS %d",
                    app.partition_count,
                    response_app.partition_count);

            for (int i = 0; i < response_app.partitions.size(); i++) {
                const dsn::partition_configuration &p = response_app.partitions[i];
                if (!p.primary.is_invalid()) {
                    auto find = tmp_map.find(p.primary);
                    if (find != tmp_map.end()) {
                        find->second.primary_count++;
                    }
                }
                for (int j = 0; j < p.secondaries.size(); j++) {
                    auto find = tmp_map.find(p.secondaries[j]);
                    if (find != tmp_map.end()) {
                        find->second.secondary_count++;
                    }
                }
            }
        }
    }

    // output as json format
    std::ostringstream out;
    dsn::utils::multi_table_printer mtp;
    dsn::utils::table_printer tp_details("details");
    tp_details.add_title("address");
    tp_details.add_column("status");
    if (detailed) {
        tp_details.add_column("replica_count");
        tp_details.add_column("primary_count");
        tp_details.add_column("secondary_count");
    }
    for (const auto &kv : tmp_map) {
        tp_details.add_row(kv.second.node_address);
        tp_details.append_data(kv.second.node_status);
        if (detailed) {
            tp_details.append_data(kv.second.primary_count + kv.second.secondary_count);
            tp_details.append_data(kv.second.primary_count);
            tp_details.append_data(kv.second.secondary_count);
        }
    }
    mtp.add(std::move(tp_details));

    dsn::utils::table_printer tp_count("summary");
    tp_count.add_row_name_and_data("total_node_count", alive_node_count + unalive_node_count);
    tp_count.add_row_name_and_data("alive_node_count", alive_node_count);
    tp_count.add_row_name_and_data("unalive_node_count", unalive_node_count);
    mtp.add(std::move(tp_count));
    mtp.output(out, dsn::utils::table_printer::output_format::kJsonCompact);

    resp.body = out.str();
    resp.status_code = http_status_code::ok;
}

void meta_http_service::get_cluster_info_handler(const http_request &req, http_response &resp)
{
    if (!redirect_if_not_primary(req, resp))
        return;

    dsn::utils::table_printer tp;
    std::ostringstream out;
    std::string meta_servers_str;
    int ms_size = _service->_opts.meta_servers.size();
    for (int i = 0; i < ms_size; i++) {
        meta_servers_str += _service->_opts.meta_servers[i].to_std_string();
        if (i != ms_size - 1) {
            meta_servers_str += ",";
        }
    }
    tp.add_row_name_and_data("meta_servers", meta_servers_str);
    tp.add_row_name_and_data("primary_meta_server", dsn_primary_address().to_std_string());
    std::string zk_hosts =
        dsn_config_get_value_string("zookeeper", "hosts_list", "", "zookeeper_hosts");
    zk_hosts.erase(std::remove_if(zk_hosts.begin(), zk_hosts.end(), ::isspace), zk_hosts.end());
    tp.add_row_name_and_data("zookeeper_hosts", zk_hosts);
    tp.add_row_name_and_data("zookeeper_root", _service->_cluster_root);
    tp.add_row_name_and_data(
        "meta_function_level",
        _meta_function_level_VALUES_TO_NAMES.find(_service->get_function_level())->second + 3);
    std::vector<std::string> balance_operation_type;
    balance_operation_type.emplace_back("detail");
    tp.add_row_name_and_data(
        "balance_operation_count",
        _service->_balancer->get_balance_operation_count(balance_operation_type));
    double primary_stddev, total_stddev;
    _service->_state->get_cluster_balance_score(primary_stddev, total_stddev);
    tp.add_row_name_and_data("primary_replica_count_stddev", primary_stddev);
    tp.add_row_name_and_data("total_replica_count_stddev", total_stddev);
    tp.output(out, dsn::utils::table_printer::output_format::kJsonCompact);

    resp.body = out.str();
    resp.status_code = http_status_code::ok;
}

bool meta_http_service::redirect_if_not_primary(const http_request &req, http_response &resp)
{
#ifdef DSN_MOCK_TEST
    return true;
#endif
    rpc_address leader;
    if (_service->_failure_detector->get_leader(&leader))
        return true;
    // set redirect response
    const std::string &service_name = req.service_method.first;
    const std::string &method_name = req.service_method.second;
    resp.location = "http://" + leader.to_std_string() + '/' + service_name + '/' + method_name;
    if (!req.query_args.empty()) {
        resp.location += '?';
        for (const auto &i : req.query_args) {
            resp.location += i.first + '=' + i.second + '&';
        }
        resp.location.pop_back(); // remove final '&'
    }
    resp.location.erase(std::remove(resp.location.begin(), resp.location.end(), '\0'),
                        resp.location.end()); // remove final '\0'
    resp.status_code = http_status_code::temporary_redirect;
    return false;
}

} // namespace replication
} // namespace dsn