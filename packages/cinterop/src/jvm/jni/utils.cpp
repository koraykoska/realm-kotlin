/*
 * Copyright 2021 Realm Inc.
 *
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

#include "utils.h"

#include <realm/unicode.hpp>
#include <sstream>
#include <iomanip>
#include <realm/util/safe_int_ops.hpp>

#include "utf8.hpp"

using namespace realm;
using namespace realm::util;
using namespace realm::jni_util;
using namespace realm::_impl;

// This assumes that 'jchar' is an integral type with at least 16
// non-sign value bits, that is, an unsigned 16-bit integer, or any
// signed or unsigned integer with more than 16 bits.
struct JcharTraits {
    static jchar to_int_type(jchar c) noexcept
    {
        return c;
    }
    static jchar to_char_type(jchar i) noexcept
    {
        return i;
    }
};

static std::string str_to_hex_error_code_to_message(size_t error_code){
    switch (error_code){
        case 1:
        case 2:
        case 3:
        case 4:
            return "Not enough output buffer space";
        case 5:
            return "Invalid first half of surrogate pair";
        case 6:
            return "Incomplete surrogate pair";
        case 7:
            return "Invalid second half of surrogate pair";
        default:
            return "Unknown";
    }
}

static std::string string_to_hex(const std::string& message, const jchar* str, size_t size, size_t error_code)
{
    std::ostringstream ret;

    ret << message << ": " << str_to_hex_error_code_to_message(error_code) << "; ";
    ret << "error_code = " << error_code << "; ";
    for (size_t i = 0; i < size; ++i) {
        ret << " 0x" << std::hex << std::setfill('0') << std::setw(4) << (int) str[i];
    }
    return ret.str();
}

static std::string string_to_hex(const std::string& message, realm::StringData& str, const char* in_begin, const char* in_end,
                                 jchar* out_curr, jchar* out_end, size_t retcode, size_t error_code)
{
    std::ostringstream ret;

    const char* s = str.data();
    ret << message << " ";
    ret << "error_code = " << error_code << "; ";
    ret << "retcode = " << retcode << "; ";
    ret << "StringData.size = " << str.size() << "; ";
    ret << "StringData.data = " << str << "; ";
    ret << "StringData as hex = ";
    for (std::string::size_type i = 0; i < str.size(); ++i)
        ret << " 0x" << std::hex << std::setfill('0') << std::setw(2) << (int)s[i];
    ret << "; ";
    ret << "in_begin = " << in_begin << "; ";
    ret << "in_end = " << in_end << "; ";
    ret << "out_curr = " << out_curr << "; ";
    ret << "out_end = " << out_end << ";";
    return ret.str();
}

jstring to_jstring(JNIEnv* env, realm::StringData str)
{
    if (str.is_null()) {
        return NULL;
    }

    // For efficiency, if the incoming UTF-8 string is sufficiently
    // small, we will attempt to store the UTF-16 output into a stack
    // allocated buffer of static size. Otherwise we will have to
    // dynamically allocate the output buffer after calculating its
    // size.

    const size_t stack_buf_size = 48;
    jchar stack_buf[stack_buf_size];
    std::unique_ptr<jchar[]> dyn_buf;

    const char* in_begin = str.data();
    const char* in_end = str.data() + str.size();
    jchar* out_begin = stack_buf;
    jchar* out_curr = stack_buf;
    jchar* out_end = stack_buf + stack_buf_size;

    typedef Utf8x16<jchar, JcharTraits> Xcode;

    if (str.size() <= stack_buf_size) {
        size_t retcode = Xcode::to_utf16(in_begin, in_end, out_curr, out_end);
        if (retcode != 0) {
            throw util::runtime_error(string_to_hex("Failure when converting short string to UTF-16", str, in_begin, in_end,
                                                    out_curr, out_end, size_t(0), retcode));
        }
        if (in_begin == in_end) {
            goto transcode_complete;
        }
    }

    {
        const char* in_begin2 = in_begin;
        size_t error_code;
        size_t size = Xcode::find_utf16_buf_size(in_begin2, in_end, error_code);
        if (in_begin2 != in_end) {
            throw util::runtime_error(string_to_hex("Failure when computing UTF-16 size", str, in_begin, in_end, out_curr,
                                                    out_end, size, error_code));
        }
        if (int_add_with_overflow_detect(size, stack_buf_size)) {
            throw util::runtime_error("String size overflow");
        }
        dyn_buf.reset(new jchar[size]);
        out_curr = std::copy(out_begin, out_curr, dyn_buf.get());
        out_begin = dyn_buf.get();
        out_end = dyn_buf.get() + size;
        size_t retcode = Xcode::to_utf16(in_begin, in_end, out_curr, out_end);
        if (retcode != 0) {
            throw util::runtime_error(string_to_hex("Failure when converting long string to UTF-16", str, in_begin, in_end,
                                                    out_curr, out_end, size_t(0), retcode));
        }
        REALM_ASSERT(in_begin == in_end);
    }

    transcode_complete : {
    jsize out_size;
    if (int_cast_with_overflow_detect(out_curr - out_begin, out_size)) {
        throw util::runtime_error("String size overflow");
    }

    return env->NewString(out_begin, out_size);
}
}


struct JStringCharsAccessor {
    JStringCharsAccessor(JNIEnv* e, jstring s, bool delete_jstring_ref_on_delete)
            : m_env(e)
            , m_string(s)
            , m_data(e->GetStringChars(s, 0))
            , m_size(get_size(e, s))
            , m_delete_jstring_ref_on_delete(delete_jstring_ref_on_delete)
    {
    }
    ~JStringCharsAccessor()
    {
        m_env->ReleaseStringChars(m_string, m_data);
        // TODO Left as opt-in to avoid inspecting all usages as part of the fix for
        //  https://github.com/realm/realm-java/pull/7232. We should consider making this the
        //  default and try to handle local refs uniformly through JavaLocalRefs or similar
        //  mechanisms.
        if (m_delete_jstring_ref_on_delete) {
            m_env->DeleteLocalRef(m_string);
        }
    }
    const jchar* data() const noexcept
    {
        return m_data;
    }
    size_t size() const noexcept
    {
        return m_size;
    }

private:
    JNIEnv* const m_env;
    const jstring m_string;
    const jchar* const m_data;
    const size_t m_size;
    const bool m_delete_jstring_ref_on_delete;

    static size_t get_size(JNIEnv* e, jstring s)
    {
        size_t size;
        if (int_cast_with_overflow_detect(e->GetStringLength(s), size))
            throw util::runtime_error("String size overflow");
        return size;
    }
};

JStringAccessor::JStringAccessor(JNIEnv* env, jstring str, bool delete_jstring_ref)
        : m_env(env)
{
    // For efficiency, if the incoming UTF-16 string is sufficiently
    // small, we will choose an UTF-8 output buffer whose size (in
    // bytes) is simply 4 times the number of 16-bit elements in the
    // input. This is guaranteed to be enough. However, to avoid
    // excessive over allocation, this is not done for larger input
    // strings.

    if (str == NULL) {
        m_is_null = true;
        return;
    }
    m_is_null = false;

    JStringCharsAccessor chars(env, str, delete_jstring_ref);

    typedef Utf8x16<jchar, JcharTraits> Xcode;
    size_t max_project_size = 48;
    REALM_ASSERT(max_project_size <= std::numeric_limits<size_t>::max() / 4);
    size_t buf_size;
    if (chars.size() <= max_project_size) {
        buf_size = chars.size() * 4;
    }
    else {
        const jchar* begin = chars.data();
        const jchar* end = begin + chars.size();
        size_t error_code;
        buf_size = Xcode::find_utf8_buf_size(begin, end, error_code);
    }
    char* tmp_char_array = new char[buf_size]; // throws
    m_data.reset(tmp_char_array, std::default_delete<char[]>());
    {
        const jchar* in_begin = chars.data();
        const jchar* in_end = in_begin + chars.size();
        char* out_begin = m_data.get();
        char* out_end = m_data.get() + buf_size;
        size_t error_code;
        if (!Xcode::to_utf8(in_begin, in_end, out_begin, out_end, error_code)) {
            throw util::invalid_argument(
                    string_to_hex("Failure when converting to UTF-8", chars.data(), chars.size(), error_code));
        }
        if (in_begin != in_end) {
            throw util::invalid_argument(
                    string_to_hex("in_begin != in_end when converting to UTF-8", chars.data(), chars.size(), error_code));
        }
        m_size = out_begin - m_data.get();
        // FIXME: Does this help on string issues? Or does it only help lldb?
        std::memset(tmp_char_array + m_size, 0, buf_size - m_size);
    }
}
