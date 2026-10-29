#pragma once

// Like CLASS_FIELD, but the offset is read from a variable, so it can be re-found at runtime (see util/Rtti.h).
#define CLASS_FIELD_DYN(type, name, offsetVar)                                          \
    __declspec(property(get = __get_field_##name, put = __set_field_##name)) type name; \
    type& __get_field_##name() {                                                        \
        return hat::member_at<type>(this, offsetVar);                                   \
    }                                                                                   \
    template<typename T>                                                                \
    void __set_field_##name(const T& value) {                                           \
        hat::member_at<type>(this, offsetVar) = value;                                  \
    }

#define CLASS_FIELD(type, name, offset)                                                 \
    __declspec(property(get = __get_field_##name, put = __set_field_##name)) type name; \
    type& __get_field_##name() {                                                        \
        return hat::member_at<type>(this, offset);                                      \
    }                                                                                   \
    template<typename T>                                                                \
    void __set_field_##name(const T& value) {                                           \
        hat::member_at<type>(this, offset) = value;                                     \
    }
