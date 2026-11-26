/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Base64.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/RefCounted.h>
#include <AK/ScopeGuard.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/JSONObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibJS/Script.h>
#include <LibURL/Parser.h>
#include <LibWeb/Bindings/ExtensionWorld.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWebCommon/Extensions/ExtensionManifest.h>

namespace Web::Bindings {

GC_DEFINE_ALLOCATOR(ExtensionRealmContext);

static Atomic<u64> s_next_api_call_id { 1 };
static Atomic<u64> s_next_listener_id { 1 };

static JS::Value json_value_to_js_value(JS::VM& vm, JS::Realm&, StringView json_text)
{
    if (json_text.is_empty())
        return JS::js_undefined();
    auto parsed = JS::JSONObject::parse_json(vm, Utf16String::from_utf8(json_text));
    if (parsed.is_error())
        return JS::js_undefined();
    return parsed.release_value();
}

static String js_value_to_json_string(JS::VM& vm, JS::Value value)
{
    if (value.is_undefined())
        return "null"_string;
    auto stringified = JS::JSONObject::stringify_impl(vm, value, JS::js_undefined(), JS::js_undefined());
    if (stringified.is_error() || !stringified.value().has_value())
        return "null"_string;
    return stringified.value()->to_utf8();
}

static ErrorOr<String> serialize_extension_message(JS::VM& vm, JS::Value value)
{
    auto serialized = HTML::structured_serialize_for_storage(vm, value);
    if (serialized.is_exception())
        return Error::from_string_literal("Value cannot be cloned for extension messaging");
    auto record = serialized.release_value();
    if (record.data.size() > 4 * MiB)
        return Error::from_string_literal("Extension message exceeds its size limit");
    return encode_base64(record.data.bytes());
}

static Optional<JS::Value> deserialize_extension_message(JS::VM& vm, JS::Realm& realm, JS::Value payload)
{
    if (!payload.is_string())
        return {};
    auto encoded = payload.as_string().utf16_string().to_utf8();
    if (encoded.bytes().size() > 6 * MiB)
        return {};
    auto bytes = decode_base64(encoded);
    if (bytes.is_error())
        return {};
    auto result = HTML::structured_deserialize(vm, HTML::StorageSerializationRecord(bytes.release_value()), realm);
    if (result.is_exception())
        return {};
    return result.release_value();
}

GC::Ref<ExtensionRealmContext> ExtensionRealmContext::create_for_content_script(
    DOM::Document& document,
    String extension_id,
    String extension_uuid,
    u64 grant_generation)
{
    auto& vm = main_thread_vm();
    auto window = GC::Ref { *document.window() };
    auto browsing_context = document.browsing_context();
    auto extension_world = vm.heap().allocate<WrapperWorld>(WrapperWorld::Type::Extension);

    GC::Ptr<JS::Object> fallback_global_this;
    auto execution_context = create_a_new_javascript_realm(
        vm,
        HTML::CrossOriginIsolationMode::None,
        [&](JS::Realm& realm) -> GC::Ref<JS::Object> {
            auto global = create_global_object_wrapper(realm, GC::Ref { *window });
            fallback_global_this = global;
            return global;
        },
        [&](JS::Realm& realm) -> GC::Ref<JS::Object> {
            if (browsing_context)
                return browsing_context->window_proxy_for(*extension_world, realm)->object();
            return *fallback_global_this;
        });

    auto& extension_realm = *execution_context->realm();
    auto intrinsics = extension_realm.create<Intrinsics>(extension_realm);
    extension_realm.set_host_defined(extension_realm.heap().allocate<HostDefined>(intrinsics, *extension_world, window->principal_realm()));
    cache_global_object_wrapper(extension_realm);
    MUST(initialize_window_web_interfaces(*window, extension_realm));

    auto context = vm.heap().allocate<ExtensionRealmContext>(
        document,
        move(extension_id),
        move(extension_uuid),
        grant_generation,
        Extensions::ContextType::ContentScript,
        "{}"_string,
        extension_world,
        move(execution_context),
        extension_realm,
        true);

    context->install_extension_namespaces(false);
    return context;
}

GC::Ref<ExtensionRealmContext> ExtensionRealmContext::create_for_extension_page(
    DOM::Document& document,
    String extension_id,
    String extension_uuid,
    String manifest_json)
{
    auto& vm = main_thread_vm();
    auto& window = *document.window();
    auto& realm = window.principal_realm();
    auto& world = host_defined_wrapper_world(realm);

    auto context = vm.heap().allocate<ExtensionRealmContext>(
        document,
        move(extension_id),
        move(extension_uuid),
        Extensions::ContextType::ExtensionPage,
        move(manifest_json),
        world,
        realm);

    context->install_extension_namespaces(true);
    return context;
}

ExtensionRealmContext::ExtensionRealmContext(
    DOM::Document& document,
    String extension_id,
    String extension_uuid,
    u64 grant_generation,
    Extensions::ContextType context_type,
    String manifest_json,
    GC::Ptr<WrapperWorld> wrapper_world,
    NonnullOwnPtr<JS::ExecutionContext> execution_context,
    GC::Ref<JS::Realm> realm,
    bool owns_realm_and_world)
    : m_document(document)
    , m_extension_id(move(extension_id))
    , m_extension_uuid(move(extension_uuid))
    , m_grant_generation(grant_generation)
    , m_context_type(context_type)
    , m_manifest_json(move(manifest_json))
    , m_owns_realm_and_world(owns_realm_and_world)
    , m_wrapper_world(wrapper_world)
    , m_execution_context(move(execution_context))
    , m_realm(realm)
{
}

ExtensionRealmContext::ExtensionRealmContext(
    DOM::Document& document,
    String extension_id,
    String extension_uuid,
    Extensions::ContextType context_type,
    String manifest_json,
    GC::Ref<WrapperWorld> wrapper_world,
    GC::Ref<JS::Realm> realm)
    : m_document(document)
    , m_extension_id(move(extension_id))
    , m_extension_uuid(move(extension_uuid))
    , m_context_type(context_type)
    , m_manifest_json(move(manifest_json))
    , m_owns_realm_and_world(false)
    , m_wrapper_world(wrapper_world)
    , m_realm(realm)
{
}

ExtensionRealmContext::~ExtensionRealmContext() = default;

void ExtensionRealmContext::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    visitor.visit(m_wrapper_world);
    visitor.visit(m_realm);
    for (auto& [call_id, pending] : m_pending_calls) {
        visitor.visit(pending.promise);
        visitor.visit(pending.callback);
    }
    for (auto& [event_key, listeners] : m_event_listeners) {
        for (auto& listener : listeners)
            visitor.visit(listener.callback);
    }
}

void ExtensionRealmContext::revoke_and_detach()
{
    if (m_revoked)
        return;
    m_revoked = true;

    auto pending = move(m_pending_calls);
    m_pending_calls.clear();
    m_event_listeners.clear();

    if (!pending.is_empty()) {
        HTML::TemporaryExecutionContext execution_context(*m_realm);
        for (auto& [call_id, entry] : pending) {
            auto err = JS::Error::create(*m_realm, "Extension context invalidated"_utf16);
            WebIDL::reject_promise(*m_realm, *entry.promise, err);
        }
    }

    if (m_owns_realm_and_world && m_wrapper_world && !m_wrapper_world->is_detached())
        m_wrapper_world->detach();
}

Optional<String> ExtensionRealmContext::evaluate_script(StringView source, StringView filename, bool serialize_result, Optional<String> func_source, Optional<String> args_json)
{
    if (m_revoked) {
        if (serialize_result)
            return R"({"error":"Extension context revoked"})"_string;
        return {};
    }

    auto& vm = main_thread_vm();
    bool pushed_custom_context = false;
    Optional<HTML::TemporaryExecutionContext> temp_context;

    if (m_execution_context) {
        vm.push_execution_context(*m_execution_context);
        pushed_custom_context = true;
    } else {
        temp_context.emplace(*m_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
    }
    ScopeGuard pop_context = [&] {
        if (pushed_custom_context) {
            auto* popped = vm.pop_execution_context();
            VERIFY(popped == m_execution_context.ptr());
        }
    };

    String effective_source;
    if (func_source.has_value()) {
        effective_source = MUST(String::formatted(
            "({})(...({}))",
            *func_source,
            args_json.value_or("[]"_string)));
    } else {
        effective_source = MUST(String::from_utf8(source));
    }

    auto utf16_source = Utf16String::from_utf8(effective_source);
    auto script_or_error = JS::Script::parse(utf16_source, *m_realm, filename);
    if (script_or_error.is_error()) {
        auto message = script_or_error.error().first().to_utf16_string();
        if (serialize_result) {
            JsonObject err_obj;
            err_obj.set("error"sv, message.to_utf8());
            return err_obj.serialized();
        }
        return {};
    }

    auto completion = vm.run(script_or_error.value());
    if (completion.is_throw_completion()) {
        auto thrown = completion.release_error().value();
        auto err_str = thrown.to_utf16_string(vm);
        String msg = err_str.is_error() ? "Script threw an exception"_string : err_str.value().to_utf8();
        if (serialize_result) {
            JsonObject err_obj;
            err_obj.set("error"sv, msg);
            return err_obj.serialized();
        }
        return {};
    }

    if (!serialize_result)
        return {};

    JsonObject ok_obj;
    auto val = completion.value();
    auto json_str = js_value_to_json_string(vm, val);
    auto parsed = JsonValue::from_string(json_str);
    if (!parsed.is_error())
        ok_obj.set("result"sv, parsed.release_value());
    else
        ok_obj.set("result"sv, JsonValue {});
    return ok_obj.serialized();
}

JS::Value ExtensionRealmContext::create_event_object(StringView ns_name, StringView event_name)
{
    auto& vm = main_thread_vm();
    auto event_obj = JS::Object::create(*m_realm, m_realm->intrinsics().object_prototype());
    auto event_key = MUST(String::formatted("{}.{}", ns_name, event_name));
    bool is_web_request = (ns_name == "webRequest"sv && event_name == "onBeforeRequest"sv);

    auto add_listener = JS::NativeFunction::create(
        *m_realm,
        GC::create_function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)>(vm.heap(), GC::captures(GC::Ref { *this }, event_key, is_web_request), [](auto& self, auto const& event_key, bool is_web_request, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (self->m_revoked)
                return JS::js_undefined();
            auto cb_val = vm.argument(0);
            if (!cb_val.is_function())
                return vm.throw_completion<JS::TypeError>("addListener requires a function"_utf16);

            u64 listener_id = s_next_listener_id.fetch_add(1);
            auto& listeners = self->m_event_listeners.ensure(event_key);
            if (any_of(listeners, [&](auto const& entry) { return entry.callback.ptr() == &cb_val.as_function(); }))
                return JS::js_undefined();
            EventListener listener {
                .callback = cb_val.as_function(),
                .listener_id = listener_id,
                .urls = {},
                .types = {},
            };

            if (is_web_request) {
                JsonObject args_obj;
                args_obj.set("listener_id"sv, listener_id);

                JsonArray urls_arr;
                JsonArray types_arr;
                if (vm.argument_count() > 1 && vm.argument(1).is_object()) {
                    auto& filter_obj = vm.argument(1).as_object();
                    auto urls_val = filter_obj.get("urls"_utf16_fly_string);
                    if (!urls_val.is_error() && urls_val.value().is_object()) {
                        auto json_urls = js_value_to_json_string(vm, urls_val.value());
                        if (auto parsed = JsonValue::from_string(json_urls); !parsed.is_error() && parsed.value().is_array())
                            urls_arr = parsed.value().as_array();
                    }
                    auto types_val = filter_obj.get("types"_utf16_fly_string);
                    if (!types_val.is_error() && types_val.value().is_object()) {
                        auto json_types = js_value_to_json_string(vm, types_val.value());
                        if (auto parsed = JsonValue::from_string(json_types); !parsed.is_error() && parsed.value().is_array())
                            types_arr = parsed.value().as_array();
                    }
                }
                bool blocking = false;
                if (vm.argument_count() > 2 && vm.argument(2).is_object()) {
                    auto json_extra = js_value_to_json_string(vm, vm.argument(2));
                    if (auto parsed = JsonValue::from_string(json_extra); !parsed.is_error() && parsed.value().is_array()) {
                        for (auto const& item : parsed.value().as_array().values()) {
                            if (item.is_string() && item.as_string() == "blocking"sv)
                                blocking = true;
                        }
                    }
                }
                if (urls_arr.is_empty())
                    return vm.throw_completion<JS::TypeError>("webRequest filter requires at least one URL pattern"_utf16);
                for (auto const& value : urls_arr.values()) {
                    if (!value.is_string())
                        return vm.throw_completion<JS::TypeError>("Invalid webRequest URL pattern"_utf16);
                    auto pattern = Extensions::MatchPattern::parse(value.as_string());
                    if (pattern.is_error())
                        return vm.throw_completion<JS::TypeError>("Invalid webRequest URL pattern"_utf16);
                    listener.urls.append(pattern.release_value());
                }
                for (auto const& value : types_arr.values()) {
                    auto type = value.is_string() ? Extensions::web_request_resource_type_from_string(value.as_string()) : Optional<Extensions::WebRequestResourceType> {};
                    if (!type.has_value())
                        return vm.throw_completion<JS::TypeError>("Invalid webRequest resource type"_utf16);
                    listener.types.append(*type);
                }
                listener.blocking = blocking;
                args_obj.set("urls"sv, move(urls_arr));
                args_obj.set("types"sv, move(types_arr));
                args_obj.set("blocking"sv, blocking);

                u64 call_id = s_next_api_call_id.fetch_add(1);
                self->m_document->page().client().page_did_call_extension_api(
                    call_id,
                    self->m_extension_id,
                    self->m_context_type,
                    self->m_document->id(),
                    self->m_grant_generation,
                    "webRequest"_string,
                    "addListener"_string,
                    args_obj.serialized());
            }

            listeners.append(move(listener));
            return JS::js_undefined();
        }),
        1, "addListener"_utf16_fly_string, m_realm);

    auto remove_listener = JS::NativeFunction::create(
        *m_realm,
        GC::create_function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)>(vm.heap(), GC::captures(GC::Ref { *this }, event_key, is_web_request), [](auto& self, auto const& event_key, bool is_web_request, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            auto cb_val = vm.argument(0);
            if (!cb_val.is_function())
                return JS::js_undefined();
            auto it = self->m_event_listeners.find(event_key);
            if (it == self->m_event_listeners.end())
                return JS::js_undefined();

            Vector<u64> removed_ids;
            it->value.remove_all_matching([&](EventListener const& entry) {
                if (entry.callback.ptr() == &cb_val.as_function()) {
                    removed_ids.append(entry.listener_id);
                    return true;
                }
                return false;
            });

            if (is_web_request) {
                for (auto listener_id : removed_ids) {
                    JsonObject args_obj;
                    args_obj.set("listener_id"sv, listener_id);
                    u64 call_id = s_next_api_call_id.fetch_add(1);
                    self->m_document->page().client().page_did_call_extension_api(
                        call_id,
                        self->m_extension_id,
                        self->m_context_type,
                        self->m_document->id(),
                        self->m_grant_generation,
                        "webRequest"_string,
                        "removeListener"_string,
                        args_obj.serialized());
                }
            }
            return JS::js_undefined();
        }),
        1, "removeListener"_utf16_fly_string, m_realm);

    auto has_listener = JS::NativeFunction::create(
        *m_realm,
        GC::create_function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)>(vm.heap(), GC::captures(GC::Ref { *this }, event_key), [](auto& self, auto const& event_key, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            auto cb_val = vm.argument(0);
            if (!cb_val.is_function())
                return JS::Value(false);
            auto it = self->m_event_listeners.find(event_key);
            if (it == self->m_event_listeners.end())
                return JS::Value(false);
            for (auto const& entry : it->value) {
                if (entry.callback.ptr() == &cb_val.as_function())
                    return JS::Value(true);
            }
            return JS::Value(false);
        }),
        1, "hasListener"_utf16_fly_string, m_realm);

    event_obj->define_direct_property("addListener"_utf16_fly_string, add_listener, JS::default_attributes);
    event_obj->define_direct_property("removeListener"_utf16_fly_string, remove_listener, JS::default_attributes);
    event_obj->define_direct_property("hasListener"_utf16_fly_string, has_listener, JS::default_attributes);
    return event_obj;
}

JS::ThrowCompletionOr<JS::Value> ExtensionRealmContext::invoke_api_call(JS::VM& vm, StringView ns_name, StringView method_name, bool is_chrome)
{
    auto promise = WebIDL::create_promise(*m_realm);
    if (m_revoked) {
        auto err = JS::Error::create(*m_realm, "Extension context invalidated"_utf16);
        WebIDL::reject_promise(*m_realm, *promise, err);
        return promise->promise();
    }

    size_t arg_count = vm.argument_count();
    GC::Ptr<JS::FunctionObject> callback;
    if (is_chrome && arg_count > 0 && vm.argument(arg_count - 1).is_function()) {
        callback = &vm.argument(arg_count - 1).as_function();
        --arg_count;
    }

    Optional<size_t> message_index;
    if (ns_name == "runtime"sv && method_name == "sendMessage"sv)
        message_index = 0;
    else if (ns_name == "tabs"sv && method_name == "sendMessage"sv)
        message_index = 1;
    if (message_index.has_value() && arg_count != *message_index + 1)
        return vm.throw_completion<JS::TypeError>("Expected a message for this extension"_utf16);

    // Special local handling for scripting.executeScript({ target, files, func, args })
    String serialized_args;
    if (ns_name == "scripting"sv && method_name == "executeScript"sv && arg_count > 0 && vm.argument(0).is_object()) {
        auto& details_obj = vm.argument(0).as_object();
        auto func_val = details_obj.get("func"_utf16_fly_string);
        if (!func_val.is_error() && func_val.value().is_function()) {
            auto func_str_or_err = func_val.value().to_utf16_string(vm);
            JsonObject wrapper;
            auto json_rest = js_value_to_json_string(vm, vm.argument(0));
            if (auto parsed = JsonValue::from_string(json_rest); !parsed.is_error() && parsed.value().is_object())
                wrapper = parsed.value().as_object();
            if (!func_str_or_err.is_error())
                wrapper.set("func_source"sv, func_str_or_err.value().to_utf8());
            JsonArray arr;
            arr.must_append(move(wrapper));
            serialized_args = arr.serialized();
        }
    }

    if (serialized_args.is_empty()) {
        JsonArray arr;
        for (size_t i = 0; i < arg_count; ++i) {
            if (i == message_index) {
                auto encoded = serialize_extension_message(vm, vm.argument(i));
                if (encoded.is_error())
                    return vm.throw_completion<JS::TypeError>("Unable to clone the extension message"_utf16);
                arr.must_append(encoded.release_value());
                continue;
            }
            auto json_item = js_value_to_json_string(vm, vm.argument(i));
            auto parsed = JsonValue::from_string(json_item);
            if (!parsed.is_error())
                arr.must_append(parsed.release_value());
            else
                arr.must_append(JsonValue {});
        }
        serialized_args = arr.serialized();
    }

    u64 call_id = s_next_api_call_id.fetch_add(1);
    m_pending_calls.set(call_id, PendingCall {
                                     .promise = promise,
                                     .callback = callback,
                                     .is_chrome = is_chrome,
                                     .clone_response = message_index.has_value(),
                                 });

    m_document->page().client().page_did_call_extension_api(
        call_id,
        m_extension_id,
        m_context_type,
        m_document->id(),
        m_grant_generation,
        MUST(String::from_utf8(ns_name)),
        MUST(String::from_utf8(method_name)),
        serialized_args);

    if (callback)
        return JS::js_undefined();
    return promise->promise();
}

JS::Value ExtensionRealmContext::create_namespace_object(StringView ns_name, ReadonlySpan<StringView> methods, bool is_chrome)
{
    auto& vm = main_thread_vm();
    auto ns_obj = JS::Object::create(*m_realm, m_realm->intrinsics().object_prototype());
    auto ns_str = MUST(String::from_utf8(ns_name));

    for (auto method : methods) {
        auto method_str = MUST(String::from_utf8(method));
        auto fn = JS::NativeFunction::create(
            *m_realm,
            GC::create_function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)>(vm.heap(), GC::captures(GC::Ref { *this }, ns_str, method_str, is_chrome), [](auto& self, auto const& ns_str, auto const& method_str, bool is_chrome, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
                return self->invoke_api_call(vm, ns_str, method_str, is_chrome);
            }),
            0, Utf16FlyString::from_utf8(method), m_realm);
        ns_obj->define_direct_property(Utf16FlyString::from_utf8(method), fn, JS::default_attributes);
    }
    return ns_obj;
}

void ExtensionRealmContext::install_extension_namespaces(bool full_extension_page_apis)
{
    auto& vm = main_thread_vm();
    auto& global = m_realm->global_object();

    auto build_root = [&](bool is_chrome) -> JS::Value {
        auto root = JS::Object::create(*m_realm, m_realm->intrinsics().object_prototype());

        // runtime
        static constexpr Array runtime_methods { "sendMessage"sv, "openOptionsPage"sv };
        auto runtime_obj = create_namespace_object("runtime"sv, runtime_methods.span(), is_chrome);
        auto& runtime_js_obj = runtime_obj.as_object();
        runtime_js_obj.define_direct_property("id"_utf16_fly_string, JS::PrimitiveString::create(vm, Utf16String::from_utf8(m_extension_id)), JS::default_attributes);

        auto get_url_fn = JS::NativeFunction::create(
            *m_realm,
            GC::create_function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)>(vm.heap(), GC::captures(GC::Ref { *this }), [](auto& self, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
                auto arg = TRY(vm.argument(0).to_utf16_string(vm));
                auto raw_path = arg.to_utf8();
                auto normalized = Extensions::ExtensionManifest::normalize_package_relative_path(raw_path);
                if (normalized.is_error())
                    return JS::PrimitiveString::create(vm, Utf16String::from_utf8(MUST(String::formatted("moz-extension://{}/", self->m_extension_uuid))));
                return JS::PrimitiveString::create(vm, Utf16String::from_utf8(MUST(String::formatted("moz-extension://{}/{}", self->m_extension_uuid, normalized.value()))));
            }),
            1, "getURL"_utf16_fly_string, m_realm);
        runtime_js_obj.define_direct_property("getURL"_utf16_fly_string, get_url_fn, JS::default_attributes);

        auto get_manifest_fn = JS::NativeFunction::create(
            *m_realm,
            GC::create_function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)>(vm.heap(), GC::captures(GC::Ref { *this }), [](auto& self, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
                return json_value_to_js_value(vm, *self->m_realm, self->m_manifest_json);
            }),
            0, "getManifest"_utf16_fly_string, m_realm);
        runtime_js_obj.define_direct_property("getManifest"_utf16_fly_string, get_manifest_fn, JS::default_attributes);

        runtime_js_obj.define_direct_property("onMessage"_utf16_fly_string, create_event_object("runtime"sv, "onMessage"sv), JS::default_attributes);
        runtime_js_obj.define_direct_property("onInstalled"_utf16_fly_string, create_event_object("runtime"sv, "onInstalled"sv), JS::default_attributes);

        auto last_error_getter = GC::create_function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)>(
            vm.heap(),
            GC::captures(GC::Ref { *this }),
            [](auto& self, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
                if (self->m_last_error.is_empty())
                    return JS::js_undefined();
                auto err_obj = JS::Object::create(*self->m_realm, self->m_realm->intrinsics().object_prototype());
                err_obj->define_direct_property("message"_utf16_fly_string, JS::PrimitiveString::create(vm, Utf16String::from_utf8(self->m_last_error)), JS::default_attributes);
                return err_obj;
            });
        runtime_js_obj.define_native_accessor(*m_realm, "lastError"_utf16_fly_string, last_error_getter, nullptr, JS::Attribute::Configurable);
        root->define_direct_property("runtime"_utf16_fly_string, runtime_obj, JS::default_attributes);

        // storage.local + storage.onChanged
        auto storage_obj = JS::Object::create(*m_realm, m_realm->intrinsics().object_prototype());
        static constexpr Array storage_local_methods { "get"sv, "set"sv, "remove"sv, "clear"sv, "getBytesInUse"sv };
        storage_obj->define_direct_property("local"_utf16_fly_string, create_namespace_object("storage.local"sv, storage_local_methods.span(), is_chrome), JS::default_attributes);
        storage_obj->define_direct_property("onChanged"_utf16_fly_string, create_event_object("storage"sv, "onChanged"sv), JS::default_attributes);
        root->define_direct_property("storage"_utf16_fly_string, storage_obj, JS::default_attributes);

        if (!full_extension_page_apis)
            return root;

        // tabs
        static constexpr Array tabs_methods { "query"sv, "get"sv, "create"sv, "update"sv, "remove"sv, "sendMessage"sv, "executeScript"sv, "insertCSS"sv, "removeCSS"sv };
        auto tabs_obj = create_namespace_object("tabs"sv, tabs_methods.span(), is_chrome);
        auto& tabs_js_obj = tabs_obj.as_object();
        tabs_js_obj.define_direct_property("onUpdated"_utf16_fly_string, create_event_object("tabs"sv, "onUpdated"sv), JS::default_attributes);
        tabs_js_obj.define_direct_property("onActivated"_utf16_fly_string, create_event_object("tabs"sv, "onActivated"sv), JS::default_attributes);
        tabs_js_obj.define_direct_property("onCreated"_utf16_fly_string, create_event_object("tabs"sv, "onCreated"sv), JS::default_attributes);
        tabs_js_obj.define_direct_property("onRemoved"_utf16_fly_string, create_event_object("tabs"sv, "onRemoved"sv), JS::default_attributes);
        root->define_direct_property("tabs"_utf16_fly_string, tabs_obj, JS::default_attributes);

        static constexpr Array window_methods { "getCurrent"sv };
        root->define_direct_property("windows"_utf16_fly_string, create_namespace_object("windows"sv, window_methods.span(), is_chrome), JS::default_attributes);

        // scripting
        static constexpr Array scripting_methods { "executeScript"sv, "insertCSS"sv, "removeCSS"sv };
        root->define_direct_property("scripting"_utf16_fly_string, create_namespace_object("scripting"sv, scripting_methods.span(), is_chrome), JS::default_attributes);

        // action + browserAction
        static constexpr Array action_methods { "setTitle"sv, "getTitle"sv, "setBadgeText"sv, "getBadgeText"sv, "setBadgeBackgroundColor"sv, "getBadgeBackgroundColor"sv, "setPopup"sv, "getPopup"sv, "setIcon"sv, "enable"sv, "disable"sv };
        auto action_obj = create_namespace_object("action"sv, action_methods.span(), is_chrome);
        action_obj.as_object().define_direct_property("onClicked"_utf16_fly_string, create_event_object("action"sv, "onClicked"sv), JS::default_attributes);
        root->define_direct_property("action"_utf16_fly_string, action_obj, JS::default_attributes);

        auto browser_action_obj = create_namespace_object("browserAction"sv, action_methods.span(), is_chrome);
        browser_action_obj.as_object().define_direct_property("onClicked"_utf16_fly_string, create_event_object("browserAction"sv, "onClicked"sv), JS::default_attributes);
        root->define_direct_property("browserAction"_utf16_fly_string, browser_action_obj, JS::default_attributes);

        // sidebarAction
        static constexpr Array sidebar_methods { "setPanel"sv, "getPanel"sv, "setTitle"sv, "getTitle"sv, "setIcon"sv, "open"sv, "close"sv, "toggle"sv, "isOpen"sv };
        root->define_direct_property("sidebarAction"_utf16_fly_string, create_namespace_object("sidebarAction"sv, sidebar_methods.span(), is_chrome), JS::default_attributes);

        // permissions
        static constexpr Array permissions_methods { "contains"sv, "getAll"sv, "remove"sv };
        auto perms_obj = create_namespace_object("permissions"sv, permissions_methods.span(), is_chrome);
        perms_obj.as_object().define_direct_property("onAdded"_utf16_fly_string, create_event_object("permissions"sv, "onAdded"sv), JS::default_attributes);
        perms_obj.as_object().define_direct_property("onRemoved"_utf16_fly_string, create_event_object("permissions"sv, "onRemoved"sv), JS::default_attributes);
        root->define_direct_property("permissions"_utf16_fly_string, perms_obj, JS::default_attributes);

        // webRequest
        auto web_request_obj = JS::Object::create(*m_realm, m_realm->intrinsics().object_prototype());
        web_request_obj->define_direct_property("onBeforeRequest"_utf16_fly_string, create_event_object("webRequest"sv, "onBeforeRequest"sv), JS::default_attributes);
        root->define_direct_property("webRequest"_utf16_fly_string, web_request_obj, JS::default_attributes);

        return root;
    };

    global.define_direct_property("browser"_utf16_fly_string, build_root(false), JS::default_attributes);
    global.define_direct_property("chrome"_utf16_fly_string, build_root(true), JS::default_attributes);
}

void ExtensionRealmContext::handle_api_response(u64 call_id, bool success, StringView result_or_error_json)
{
    auto pending = m_pending_calls.take(call_id);
    if (!pending.has_value() || m_revoked)
        return;

    auto& vm = main_thread_vm();
    bool pushed_custom_context = false;
    Optional<HTML::TemporaryExecutionContext> temp_context;
    if (m_execution_context) {
        vm.push_execution_context(*m_execution_context);
        pushed_custom_context = true;
    } else {
        temp_context.emplace(*m_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
    }
    ScopeGuard pop_context = [&] {
        if (pushed_custom_context) {
            auto* popped = vm.pop_execution_context();
            VERIFY(popped == m_execution_context.ptr());
        }
    };

    auto decode_response = [&]() {
        auto value = json_value_to_js_value(vm, *m_realm, result_or_error_json);
        if (!pending->clone_response)
            return value;
        return deserialize_extension_message(vm, *m_realm, value).value_or(JS::js_undefined());
    };
    if (pending->callback) {
        if (success) {
            m_last_error = {};
            auto arg = decode_response();
            (void)JS::call(vm, *pending->callback, JS::js_undefined(), arg);
        } else {
            m_last_error = MUST(String::from_utf8(result_or_error_json));
            (void)JS::call(vm, *pending->callback, JS::js_undefined());
            m_last_error = {};
        }
        return;
    }

    if (success) {
        auto val = decode_response();
        WebIDL::resolve_promise(*m_realm, *pending->promise, val);
    } else {
        auto err = JS::Error::create(*m_realm, Utf16String::from_utf8(result_or_error_json));
        WebIDL::reject_promise(*m_realm, *pending->promise, err);
    }
}

void ExtensionRealmContext::dispatch_event(StringView namespace_name, StringView event_name, StringView args_json, Optional<u64> reply_id)
{
    if (m_revoked) {
        if (reply_id.has_value())
            m_document->page().client().page_did_reply_extension_event(*reply_id, false, "null"_string);
        return;
    }

    auto event_key = MUST(String::formatted("{}.{}", namespace_name, event_name));
    auto it = m_event_listeners.find(event_key);
    if (it == m_event_listeners.end() || it->value.is_empty()) {
        if (reply_id.has_value())
            m_document->page().client().page_did_reply_extension_event(*reply_id, false, "null"_string);
        return;
    }

    auto& vm = main_thread_vm();
    bool pushed_custom_context = false;
    Optional<HTML::TemporaryExecutionContext> temp_context;
    if (m_execution_context) {
        vm.push_execution_context(*m_execution_context);
        pushed_custom_context = true;
    } else {
        temp_context.emplace(*m_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
    }
    ScopeGuard pop_context = [&] {
        if (pushed_custom_context) {
            auto* popped = vm.pop_execution_context();
            VERIFY(popped == m_execution_context.ptr());
        }
    };

    auto args_val = json_value_to_js_value(vm, *m_realm, args_json);
    Vector<JS::Value> args;
    if (args_val.is_object() && is<JS::Array>(args_val.as_object())) {
        auto& arr = static_cast<JS::Array&>(args_val.as_object());
        auto len_val = arr.get("length"_utf16_fly_string);
        size_t len = (!len_val.is_error() && len_val.value().is_number()) ? static_cast<size_t>(len_val.value().as_double()) : 0;
        for (size_t i = 0; i < len; ++i)
            args.append(MUST(arr.get(i)));
    } else if (!args_val.is_undefined()) {
        args.append(args_val);
    }

    auto listeners = it->value;

    if (event_key == "runtime.onMessage"sv) {
        auto message = args.is_empty() ? Optional<JS::Value> {} : deserialize_extension_message(vm, *m_realm, args[0]);
        if (!message.has_value()) {
            if (reply_id.has_value())
                m_document->page().client().page_did_reply_extension_event(*reply_id, false, "null"_string);
            return;
        }
        args[0] = *message;
        struct MessageReplyState : public RefCounted<MessageReplyState> {
            GC_HOLDS_NO_CELL_EDGES;
            bool replied { false };
        };
        auto reply_state = make_ref_counted<MessageReplyState>();
        bool async_pending = false;

        auto send_response_fn = JS::NativeFunction::create(
            *m_realm,
            GC::create_function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)>(vm.heap(), GC::captures(GC::Ref { *this }, reply_id, reply_state), [](auto& self, auto reply_id, auto& reply_state, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
                if (reply_state->replied)
                    return JS::js_undefined();
                reply_state->replied = true;
                if (reply_id.has_value()) {
                    auto encoded = serialize_extension_message(vm, vm.argument(0));
                    bool sent = !encoded.is_error();
                    auto response = sent ? JsonValue(encoded.release_value()).serialized() : "null"_string;
                    self->m_document->page().client().page_did_reply_extension_event(*reply_id, sent, response);
                }
                return JS::js_undefined();
            }),
            1, "sendResponse"_utf16_fly_string, m_realm);

        while (args.size() < 2)
            args.append(JS::js_undefined());
        args.append(send_response_fn);

        for (auto const& listener : listeners) {
            auto result = JS::call(vm, *listener.callback, JS::js_undefined(), args.span());
            if (result.is_throw_completion())
                continue;
            auto val = result.release_value();
            if (val.is_boolean() && val.as_bool()) {
                async_pending = true;
                break;
            }
            if (val.is_object() && is<JS::Promise>(val.as_object())) {
                async_pending = true;
                auto& js_promise = static_cast<JS::Promise&>(val.as_object());
                auto capability = WebIDL::create_promise(*m_realm);
                auto resolve_fn = JS::NativeFunction::create(
                    *m_realm,
                    GC::create_function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)>(vm.heap(), GC::captures(GC::Ref { *this }, reply_id, reply_state), [](auto& self, auto reply_id, auto& reply_state, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
                        if (reply_state->replied)
                            return JS::js_undefined();
                        reply_state->replied = true;
                        if (reply_id.has_value()) {
                            auto encoded = serialize_extension_message(vm, vm.argument(0));
                            bool sent = !encoded.is_error();
                            auto response = sent ? JsonValue(encoded.release_value()).serialized() : "null"_string;
                            self->m_document->page().client().page_did_reply_extension_event(*reply_id, sent, response);
                        }
                        return JS::js_undefined();
                    }),
                    1);
                auto reject_fn = JS::NativeFunction::create(
                    *m_realm,
                    GC::create_function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)>(vm.heap(), GC::captures(GC::Ref { *this }, reply_id, reply_state), [](auto& self, auto reply_id, auto& reply_state, JS::VM&) -> JS::ThrowCompletionOr<JS::Value> {
                        if (reply_state->replied)
                            return JS::js_undefined();
                        reply_state->replied = true;
                        if (reply_id.has_value())
                            self->m_document->page().client().page_did_reply_extension_event(*reply_id, false, "null"_string);
                        return JS::js_undefined();
                    }),
                    1);
                (void)js_promise.perform_then(resolve_fn, reject_fn, capability);
                break;
            }
            if (reply_state->replied)
                break;
        }

        if (!reply_state->replied && !async_pending && reply_id.has_value())
            m_document->page().client().page_did_reply_extension_event(*reply_id, false, "null"_string);
        return;
    }

    if (event_key == "webRequest.onBeforeRequest"sv) {
        auto parsed = JsonValue::from_string(args_json);
        auto const& details = parsed.value().as_array().at(0).as_object();
        auto url = URL::Parser::basic_parse(details.get_string("url"sv).value_or({}));
        auto type = Extensions::web_request_resource_type_from_string(details.get_string("type"sv).value_or({}));

        struct DecisionState : public RefCounted<DecisionState> {
            GC_HOLDS_NO_CELL_EDGES;
            size_t pending { 1 };
            bool cancel { false };
            void complete(ExtensionRealmContext& context, u64 reply_id, JS::Value value)
            {
                if (value.is_object()) {
                    auto cancellation = value.as_object().get("cancel"_utf16_fly_string);
                    if (!cancellation.is_error() && cancellation.value().to_boolean())
                        cancel = true;
                }
                if (--pending != 0 || context.is_revoked())
                    return;
                JsonObject result;
                result.set("cancel"sv, cancel);
                context.m_document->page().client().page_did_reply_extension_event(reply_id, true, result.serialized());
            }
        };
        auto state = make_ref_counted<DecisionState>();

        for (auto const& listener : listeners) {
            if (!url.has_value() || !any_of(listener.urls, [&](auto const& pattern) { return pattern.matches(*url); }))
                continue;
            if (!listener.types.is_empty() && (!type.has_value() || !listener.types.contains_slow(*type)))
                continue;
            auto result = JS::call(vm, *listener.callback, JS::js_undefined(), args.span());
            if (result.is_throw_completion() || !listener.blocking || !reply_id.has_value())
                continue;
            auto val = result.release_value();
            ++state->pending;
            if (val.is_object() && is<JS::Promise>(val.as_object())) {
                auto& promise = static_cast<JS::Promise&>(val.as_object());
                auto resolve = JS::NativeFunction::create(*m_realm,
                    GC::create_function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)>(vm.heap(), GC::captures(GC::Ref { *this }, state, *reply_id), [](auto& self, auto& state, u64 id, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
                        state->complete(*self, id, vm.argument(0));
                        return JS::js_undefined();
                    }),
                    1);
                auto reject = JS::NativeFunction::create(*m_realm,
                    GC::create_function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)>(vm.heap(), GC::captures(GC::Ref { *this }, state, *reply_id), [](auto& self, auto& state, u64 id, JS::VM&) -> JS::ThrowCompletionOr<JS::Value> {
                        state->complete(*self, id, JS::js_undefined());
                        return JS::js_undefined();
                    }),
                    1);
                (void)promise.perform_then(resolve, reject, WebIDL::create_promise(*m_realm));
            } else {
                state->complete(*this, *reply_id, val);
            }
        }
        if (reply_id.has_value())
            state->complete(*this, *reply_id, JS::js_undefined());
        return;
    }

    for (auto const& listener : listeners)
        (void)JS::call(vm, *listener.callback, JS::js_undefined(), args.span());

    if (reply_id.has_value())
        m_document->page().client().page_did_reply_extension_event(*reply_id, true, "null"_string);
}

}
