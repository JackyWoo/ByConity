/*
 * Copyright 2016-2023 ClickHouse, Inc.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


/*
 * This file may have been modified by Bytedance Ltd. and/or its affiliates (“ Bytedance's Modifications”).
 * All Bytedance's Modifications are Copyright (2023) Bytedance Ltd. and/or its affiliates.
 */

#pragma once

#include <map>
#include <unordered_set>
#include <vector>
#include <common/types.h>

namespace DB
{
/**
  * Various tweaks for input/output formats. Text serialization/deserialization
  * of data types also depend on some of these settings. It is different from
  * FormatFactorySettings in that it has all necessary user-provided settings
  * combined with information from context etc, that we can use directly during
  * serialization. In contrast, FormatFactorySettings' job is to reflect the
  * changes made to user-visible format settings, such as when tweaking the
  * the format for File engine.
  * NOTE Parameters for unrelated formats and unrelated data types are collected
  * in this struct - it prevents modularity, but they are difficult to separate.
  */
struct FormatSettings
{
    /// Format will be used for streaming. Not every formats support it
    /// Option means that each chunk of data need to be formatted independently. Also each chunk will be flushed at the end of processing.
    bool enable_streaming = false;

    bool skip_unknown_fields = false;
    bool with_names_use_header = false;
    bool write_statistics = true;
    bool import_nested_json = false;
    bool null_as_default = true;
    bool defaults_for_omitted_fields = true;
    enum class DateTimeInputFormat
    {
        Basic, /// Default format for fast parsing: YYYY-MM-DD hh:mm:ss (ISO-8601 without fractional part and timezone) or NNNNNNNNNN unix timestamp.
        BestEffort /// Use sophisticated rules to parse whatever possible.
    };

    DateTimeInputFormat date_time_input_format = DateTimeInputFormat::Basic;

    enum class DateTimeOutputFormat
    {
        Simple,
        ISO,
        UnixTimestamp
    };

    DateTimeOutputFormat date_time_output_format = DateTimeOutputFormat::Simple;

    UInt64 input_allow_errors_num = 0;
    Float32 input_allow_errors_ratio = 0;

    struct
    {
        UInt64 row_group_size = 1000000;
        bool low_cardinality_as_dictionary = false;
        bool allow_missing_columns = false;
    } arrow;

    struct
    {
        String schema_registry_url;
        String output_codec;
        UInt64 output_sync_interval = 16 * 1024;
        bool allow_missing_fields = false;
    } avro;

    String bool_true_representation = "true";
    String bool_false_representation = "false";

    struct CSV
    {
        char delimiter = ',';
        bool allow_single_quotes = true;
        bool allow_double_quotes = true;
        bool unquoted_null_literal_as_null = false;
        bool empty_as_default = false;
        bool crlf_end_of_line = false;
        bool input_format_enum_as_number = false;
        bool input_format_arrays_as_nested_csv = false;
    } csv;

    struct Custom
    {
        std::string result_before_delimiter;
        std::string result_after_delimiter;
        std::string row_before_delimiter;
        std::string row_after_delimiter;
        std::string row_between_delimiter;
        std::string field_delimiter;
        std::string escaping_rule;
    } custom;

    struct
    {
        bool array_of_rows = false;
        bool quote_64bit_integers = false;
        bool quote_denormals = true;
        bool escape_forward_slashes = true;
        bool named_tuples_as_objects = false;
        bool serialize_as_strings = false;
        bool read_bools_as_numbers = true;
        bool quota_json_string = true;
    } json;

    struct
    {
        UInt64 row_group_size = 1000000;
        bool import_nested = false;
        bool allow_missing_columns = false;
        std::unordered_set<int> skip_row_groups;
        bool preserve_order = false;
        size_t file_size = 0 ;
        bool case_insensitive_column_matching = false;
        UInt64 max_block_size = 8192;
    } parquet;

    struct Orc
    {
        bool allow_missing_columns = false;
        std::vector<bool> skip_stripes;
    } orc;

    struct Pretty
    {
        UInt64 max_rows = 10000;
        UInt64 max_column_pad_width = 250;
        UInt64 max_value_width = 10000;
        bool color = true;

        bool output_format_pretty_row_numbers = false;

        enum class Charset
        {
            UTF8,
            ASCII,
        };

        Charset charset = Charset::UTF8;
    } pretty;

    struct
    {
        /**
         * Some buffers (kafka / rabbit) split the rows internally using callback,
         * and always send one row per message, so we can push there formats
         * without framing / delimiters (like ProtobufSingle). In other cases,
         * we have to enforce exporting at most one row in the format output,
         * because Protobuf without delimiters is not generally useful.
         */
        bool allow_multiple_rows_without_delimiter = false;
        /**
         * Some buffers like kafka only has one row each message without length
         *  delimiter. To be compatible with previous version, add this setting
         * to force ProtobufReader only consume one row in each buffer.
         */
        bool enable_multiple_message = false;
        /**
         * Only has meaning when enable_multiple_message is set to true.
         * When set to true, parse a varint header as message length. Otherwise, a 8 bytes fixed length header
         * will be read before each message.
         */
        bool default_length_parser = false;
    } protobuf;

    struct
    {
        uint32_t client_capabilities = 0;
        size_t max_packet_size = 0;
        uint8_t * sequence_id = nullptr; /// Not null if it's MySQLWire output format used to handle MySQL protocol connections.
    } mysql_wire;

    struct
    {
        std::string regexp;
        std::string escaping_rule;
        bool skip_unmatched = false;
    } regexp;

    struct
    {
        bool parse_null_map_as_empty = true;
        bool skip_null_map_value = true;
        /**
         *  Because of the maximum filename length limit (255) of ext4 filesystem and the HTTP encode of map key.
         *  The maximum key length is approximately 255/3 ~= 85, which, in fact is not abosultely safe after
         *  accounting the map name and some auxiliary symbols.
         *
         *  Pick up a smaller and lucky number, hopes it work at most time.
         */
        uint64_t max_map_key_length = 80;
    } map;

    struct
    {
        std::string format_schema;
        std::string format_schema_path;
        bool is_server = false;
    } schema;

    struct
    {
        String resultset_format;
        String row_format;
        String row_between_delimiter;
    } template_settings;

    struct
    {
        bool empty_as_default = false;
        bool crlf_end_of_line = false;
        String null_representation = "\\N";
        bool input_format_enum_as_number = false;
    } tsv;

    struct
    {
        bool interpret_expressions = true;
        bool deduce_templates_of_expressions = true;
        bool accurate_types_of_literals = true;
    } values;
};

}
