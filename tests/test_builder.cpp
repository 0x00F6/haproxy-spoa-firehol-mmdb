#include "mmdb_builder/builder.hpp"
#include <maxminddb.h>
#include <cassert>
#include <iostream>
#include <string>
#include <cmath>
#include <filesystem>
#include <thread>
#include <vector>

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "FAIL: " << msg << std::endl; \
            return 1; \
        } \
    } while (0)

using namespace mmdb_builder;

int main() {
    Builder builder(6, "TestDB");

    auto worker_func = [&](int start, int end, int t_id) {
        for (int i = start; i < end; ++i) {
            Object obj;
            obj["thread_id"] = Data(uint32_t(t_id));
            obj["val"] = Data(uint32_t(i));
            std::string ip = "10.0." + std::to_string(t_id) + "." + std::to_string(i) + "/32";
            builder.insert_network(ip, Data(obj));
        }
    };

    // Parallel insertion testing
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back(worker_func, 1, 50, t);
    }
    for (auto& th : threads) {
        th.join();
    }

    Object all_types;
    all_types["string_literal"] = Data("hello");
    all_types["string_std"] = Data(std::string("world"));
    all_types["uint16"] = Data(uint16_t(16));
    all_types["uint32"] = Data(uint32_t(32));
    all_types["int32"] = Data(int32_t(-42));
    all_types["uint64"] = Data(uint64_t(64));
    all_types["bool_true"] = Data(true);
    all_types["bool_false"] = Data(false);
    all_types["float_val"] = Data(3.14f);
    all_types["double_val"] = Data(2.71828);

    Array string_array;
    string_array.push_back(Data("array_item1"));
    string_array.push_back(Data(std::string("array_item2")));
    all_types["str_array"] = Data(string_array);

    Object nested_obj;
    nested_obj["nested_key"] = Data("nested_value");
    all_types["nested"] = Data(nested_obj);

    // Insert network with all types included
    bool r = builder.insert_network("192.168.10.0/24", Data(all_types));
    CHECK(r, "Insert /24");

    // Testing deep merge
    Object override_obj;
    override_obj["uint16"] = Data(uint16_t(99)); // Overwrite
    override_obj["new_field"] = Data("merged"); // New field
    
    // Add it to a more specific subnet to override
    r = builder.insert_network("192.168.10.128/25", Data(override_obj));
    CHECK(r, "Insert /25 override");

    const char* filename = "test_builder_output.mmdb";
    // Build triggers parallel pruning, parallel collection, and chunked streaming IO natively
    builder.build(filename);

    MMDB_s mmdb;
    int status = MMDB_open(filename, MMDB_MODE_MMAP, &mmdb);
    CHECK(status == MMDB_SUCCESS, "Opened MMDB");

    auto check_ip = [&](const char* ip, bool expect_override) {
        int gai_error, mmdb_error;
        MMDB_lookup_result_s result = MMDB_lookup_string(&mmdb, ip, &gai_error, &mmdb_error);
        CHECK(gai_error == 0, "Lookup IP gai");
        CHECK(mmdb_error == MMDB_SUCCESS, "Lookup IP mmdb");
        CHECK(result.found_entry, "Entry found");

        MMDB_entry_data_s entry_data;
        
        status = MMDB_get_value(&result.entry, &entry_data, "string_literal", NULL);
        CHECK(status == MMDB_SUCCESS && entry_data.type == MMDB_DATA_TYPE_UTF8_STRING, "string_literal type");
        CHECK(std::string_view(entry_data.utf8_string, entry_data.data_size) == "hello", "string_literal value");

        status = MMDB_get_value(&result.entry, &entry_data, "uint16", NULL);
        CHECK(status == MMDB_SUCCESS && entry_data.type == MMDB_DATA_TYPE_UINT16, "uint16 type");
        if (expect_override) {
            CHECK(entry_data.uint16 == 99, "uint16 value override");
        } else {
            CHECK(entry_data.uint16 == 16, "uint16 value");
        }

        status = MMDB_get_value(&result.entry, &entry_data, "int32", NULL);
        CHECK(status == MMDB_SUCCESS && entry_data.type == MMDB_DATA_TYPE_INT32, "int32 type");
        CHECK(entry_data.int32 == -42, "int32 value");

        status = MMDB_get_value(&result.entry, &entry_data, "uint64", NULL);
        CHECK(status == MMDB_SUCCESS && entry_data.type == MMDB_DATA_TYPE_UINT64, "uint64 type");
        CHECK(entry_data.uint64 == 64, "uint64 value");

        status = MMDB_get_value(&result.entry, &entry_data, "bool_true", NULL);
        CHECK(status == MMDB_SUCCESS && entry_data.type == MMDB_DATA_TYPE_BOOLEAN, "bool_true type");
        CHECK(entry_data.boolean == true, "bool_true value");

        status = MMDB_get_value(&result.entry, &entry_data, "bool_false", NULL);
        CHECK(status == MMDB_SUCCESS && entry_data.type == MMDB_DATA_TYPE_BOOLEAN, "bool_false type");
        CHECK(entry_data.boolean == false, "bool_false value");

        status = MMDB_get_value(&result.entry, &entry_data, "float_val", NULL);
        CHECK(status == MMDB_SUCCESS && entry_data.type == MMDB_DATA_TYPE_FLOAT, "float type");
        CHECK(std::abs(entry_data.float_value - 3.14f) < 0.001, "float value");

        status = MMDB_get_value(&result.entry, &entry_data, "double_val", NULL);
        CHECK(status == MMDB_SUCCESS && entry_data.type == MMDB_DATA_TYPE_DOUBLE, "double type");
        CHECK(std::abs(entry_data.double_value - 2.71828) < 0.001, "double value");

        status = MMDB_get_value(&result.entry, &entry_data, "str_array", "0", NULL);
        CHECK(status == MMDB_SUCCESS && entry_data.type == MMDB_DATA_TYPE_UTF8_STRING, "array[0] type");
        CHECK(std::string_view(entry_data.utf8_string, entry_data.data_size) == "array_item1", "array[0] value");

        status = MMDB_get_value(&result.entry, &entry_data, "str_array", "1", NULL);
        CHECK(status == MMDB_SUCCESS && entry_data.type == MMDB_DATA_TYPE_UTF8_STRING, "array[1] type");
        CHECK(std::string_view(entry_data.utf8_string, entry_data.data_size) == "array_item2", "array[1] value");

        status = MMDB_get_value(&result.entry, &entry_data, "nested", "nested_key", NULL);
        CHECK(status == MMDB_SUCCESS && entry_data.type == MMDB_DATA_TYPE_UTF8_STRING, "nested_key type");
        CHECK(std::string_view(entry_data.utf8_string, entry_data.data_size) == "nested_value", "nested_key value");

        if (expect_override) {
            status = MMDB_get_value(&result.entry, &entry_data, "new_field", NULL);
            CHECK(status == MMDB_SUCCESS && entry_data.type == MMDB_DATA_TYPE_UTF8_STRING, "new_field type");
            CHECK(std::string_view(entry_data.utf8_string, entry_data.data_size) == "merged", "new_field value");
        } else {
            status = MMDB_get_value(&result.entry, &entry_data, "new_field", NULL);
            CHECK(status != MMDB_SUCCESS, "new_field should not exist");
        }

        return 0;
    };

    if (check_ip("192.168.10.5", false) != 0) return 1;    // In /24 but not /25
    if (check_ip("192.168.10.130", true) != 0) return 1;   // In /25 (override)
    
    // Check parallel inserted IPs
    for (int t = 0; t < 4; ++t) {
        for (int i = 1; i < 50; ++i) {
            std::string ip = "10.0." + std::to_string(t) + "." + std::to_string(i);
            int gai_error, mmdb_error;
            MMDB_lookup_result_s result = MMDB_lookup_string(&mmdb, ip.c_str(), &gai_error, &mmdb_error);
            CHECK(gai_error == 0 && mmdb_error == MMDB_SUCCESS && result.found_entry, "Multi-thread IP entry");
            
            MMDB_entry_data_s entry_data;
            status = MMDB_get_value(&result.entry, &entry_data, "val", NULL);
            CHECK(status == MMDB_SUCCESS && entry_data.type == MMDB_DATA_TYPE_UINT32 && entry_data.uint32 == static_cast<uint32_t>(i), "Multi-thread IP value");
        }
    }

    MMDB_close(&mmdb);
    std::filesystem::remove(filename);

    // Even a small database can fail only when the buffered output is flushed.
    Builder failing_output_builder(6, "WriteFailureTestDB");
    CHECK(failing_output_builder.insert_network("192.0.2.1/32", Data("blocked")),
          "Insert network for output error test");
    bool write_failed = false;
    try {
        failing_output_builder.build("/dev/full");
    } catch (const std::ios_base::failure&) {
        write_failed = true;
    }
    CHECK(write_failed, "MMDB build must report buffered output errors");

    std::cout << "All builder tests & parallel execution passed!" << std::endl;
    return 0;
}
