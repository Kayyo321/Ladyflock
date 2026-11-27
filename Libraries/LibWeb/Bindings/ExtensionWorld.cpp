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
#include <AK/Platform.h>
#include <AK/Random.h>
#include <AK/RefCounted.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <LibJS/Runtime/AbstractOperations.h>
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
#include <LibURL/PublicSuffixData.h>
#include <LibUnicode/IDNA.h>
#include <LibWeb/Bindings/ExtensionWorld.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Extensions/ExtensionContentScriptHost.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWebCommon/Extensions/ExtensionManifest.h>
#include <LibWebCommon/Extensions/SupportMatrix.h>

namespace Web::Bindings {

GC_DEFINE_ALLOCATOR(ExtensionRealmContext);

static Atomic<u64> s_next_api_call_id { 1 };
static Atomic<u64> s_next_listener_id { 1 };

static bool manifest_has_permission(JsonValue const& manifest, StringView permission)
{
    auto permissions = manifest.is_object() ? manifest.as_object().get_array("permissions"sv) : Optional<JsonArray const&> {};
    return permissions.has_value() && any_of(permissions->values(), [&](auto const& value) {
        return value.is_string() && value.as_string() == permission;
    });
}

static JS::Value make_public_suffix_api(JS::Realm& realm)
{
    auto& vm = realm.vm();
    auto api = JS::Object::create(realm, realm.intrinsics().object_prototype());
    for (auto method : { "isKnownSuffix"sv, "getKnownSuffix"sv, "getDomain"sv }) {
        auto name = Utf16FlyString::from_utf8(method);
        auto function = JS::NativeFunction::create(realm,
            GC::create_function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)>(vm.heap(), GC::captures(MUST(String::from_utf8(method))), [](String const& method, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
                using SuffixData = URL::PublicSuffixData;
                using StarRule = SuffixData::IncludeStarRule;
                if (!vm.argument(0).is_string())
                    return vm.throw_completion<JS::TypeError>("Expected a hostname"_utf16);
                auto input = vm.argument(0).as_string().utf16_string().to_utf8();
                auto host = URL::Parser::parse_host(input);
                if (!host.has_value() || host->is_empty_host() || input.starts_with('.') || input.contains(".."sv) || input.contains('*') || input.contains('%'))
                    return vm.throw_completion<JS::TypeError>("Invalid hostname"_utf16);

                auto make_string = [&](String const& value) {
                    return JS::Value(JS::PrimitiveString::create(vm, Utf16String::from_utf8(value)));
                };
                if (method == "isKnownSuffix"sv)
                    return JS::Value(SuffixData::is_matching_public_suffix(*host, StarRule::No));
                if (method == "getKnownSuffix"sv) {
                    auto suffix = SuffixData::find_matching_public_suffix(*host, StarRule::No);
                    return suffix.has_value() ? make_string(*suffix) : JS::js_null();
                }

                auto options = vm.argument(1);
                if (!options.is_nullish() && !options.is_object())
                    return vm.throw_completion<JS::TypeError>("Expected publicSuffix options"_utf16);
                auto read_option = [&](Utf16FlyString const& key) -> JS::ThrowCompletionOr<JS::Value> {
                    if (options.is_object())
                        return options.as_object().get(key);
                    return JS::js_undefined();
                };
                auto allow_ip_address = TRY(read_option("allowIPAddress"_utf16_fly_string));
                auto allow_plain_suffix = TRY(read_option("allowPlainSuffix"_utf16_fly_string));
                auto allow_unknown_suffix = TRY(read_option("allowUnknownSuffix"_utf16_fly_string));
                auto encoding = TRY(read_option("encoding"_utf16_fly_string));
                for (auto value : { allow_ip_address, allow_plain_suffix, allow_unknown_suffix }) {
                    if (!value.is_undefined() && !value.is_boolean())
                        return vm.throw_completion<JS::TypeError>("Expected a boolean publicSuffix option"_utf16);
                }
                bool use_display = false;
                if (!encoding.is_undefined()) {
                    if (!encoding.is_string())
                        return vm.throw_completion<JS::TypeError>("Invalid publicSuffix encoding"_utf16);
                    auto value = encoding.as_string().utf16_string();
                    use_display = value == "display"_utf16;
                    if (!use_display && value != "punycode"_utf16)
                        return vm.throw_completion<JS::TypeError>("Invalid publicSuffix encoding"_utf16);
                }

                if (!host->is_domain())
                    return allow_ip_address.to_boolean() ? make_string(input) : JS::js_null();
                auto domain = SuffixData::find_matching_registrable_domain(*host, StarRule::Yes);
                if (!domain.has_value() && allow_plain_suffix.to_boolean())
                    domain = host->serialize();
                else if (!allow_unknown_suffix.to_boolean() && !SuffixData::find_matching_public_suffix(*host, StarRule::No).has_value())
                    return JS::js_null();
                if (!domain.has_value())
                    return JS::js_null();
                return make_string(use_display ? Unicode::IDNA::to_display(*domain) : *domain);
            }),
            1, name, realm);
        api->define_direct_property(name, function, JS::default_attributes);
    }
    return api;
}

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
    auto extension_world = vm.heap().allocate<WrapperWorld>(WrapperWorld::Type::Extension);

    auto execution_context = create_a_new_javascript_realm(
        vm,
        HTML::CrossOriginIsolationMode::None,
        [&](JS::Realm& realm) -> GC::Ref<JS::Object> {
            return create_global_object_wrapper(realm, window);
        },
        [&](JS::Realm& realm) -> GC::Ref<JS::Object> {
            // Content scripts can run before their document becomes active. Their global must remain bound
            // to that document's Window throughout navigation of the browsing context.
            auto proxy = HTML::WindowProxy::create(realm);
            proxy->set_window(window);
            return proxy->object();
        });

    auto& extension_realm = *execution_context->realm();
    auto intrinsics = extension_realm.create<Intrinsics>(extension_realm);
    extension_realm.set_host_defined(extension_realm.heap().allocate<HostDefined>(intrinsics, *extension_world, window->principal_realm()));
    cache_global_object_wrapper(extension_realm);
    MUST(initialize_window_web_interfaces(*window, extension_realm));

    auto metadata = Extensions::ExtensionContentScriptHost::extension_metadata(extension_id);
    auto manifest_json = metadata.has_value() ? metadata->manifest_json : "{}"_string;
    auto locale_messages = metadata.has_value() ? JsonValue::from_string(metadata->locale_messages_json) : ErrorOr<JsonValue> { JsonValue {} };

    auto context = vm.heap().allocate<ExtensionRealmContext>(
        document,
        move(extension_id),
        move(extension_uuid),
        grant_generation,
        Extensions::ContextType::ContentScript,
        move(manifest_json),
        extension_world,
        move(execution_context),
        extension_realm,
        true);

    if (!locale_messages.is_error() && locale_messages.value().is_object())
        context->m_locale_messages = locale_messages.release_value().as_object();
    context->install_extension_namespaces(false);
    return context;
}

GC::Ref<ExtensionRealmContext> ExtensionRealmContext::create_for_extension_page(
    DOM::Document& document,
    String extension_id,
    String extension_uuid,
    String manifest_json,
    String locale_messages_json)
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

    if (auto messages = JsonValue::from_string(locale_messages_json); !messages.is_error() && messages.value().is_object())
        context->m_locale_messages = messages.release_value().as_object();
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
    for (auto& [id, port] : m_runtime_ports)
        visitor.visit(port);
}

void ExtensionRealmContext::revoke_and_detach()
{
    if (m_revoked)
        return;
    m_revoked = true;

    auto pending = move(m_pending_calls);
    m_pending_calls.clear();
    m_event_listeners.clear();
    m_runtime_ports.clear();

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
    // DOM APIs need the owning Window's settings object as their entry context even when JS
    // runs in the isolated realm.
    HTML::TemporaryExecutionContext entry_context(*m_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
    if (m_execution_context)
        vm.push_execution_context(*m_execution_context);
    ScopeGuard pop_context = [&] {
        if (m_execution_context) {
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

bool ExtensionRealmContext::has_manifest_permission(StringView permission) const
{
    auto manifest = JsonValue::from_string(m_manifest_json);
    return !manifest.is_error() && manifest_has_permission(manifest.value(), permission);
}

JS::Value ExtensionRealmContext::create_event_object(StringView ns_name, StringView event_name)
{
    auto& vm = main_thread_vm();
    auto event_obj = JS::Object::create(*m_realm, m_realm->intrinsics().object_prototype());
    auto event_key = MUST(String::formatted("{}.{}", ns_name, event_name));
    auto web_request_event = ns_name == "webRequest"sv ? Extensions::web_request_event_from_string(event_name) : Optional<Extensions::WebRequestEvent> {};

    auto add_listener = JS::NativeFunction::create(
        *m_realm,
        GC::create_function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)>(vm.heap(), GC::captures(GC::Ref { *this }, event_key, web_request_event), [](auto& self, auto const& event_key, auto web_request_event, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
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

            if (event_key.bytes_as_string_view().starts_with("webNavigation."sv) && vm.argument_count() > 1 && !vm.argument(1).is_undefined()) {
                auto value = JsonValue::from_string(js_value_to_json_string(vm, vm.argument(1)));
                if (value.is_error())
                    return vm.throw_completion<JS::TypeError>("Invalid navigation filter"_utf16);
                auto filter = Extensions::NavigationURLFilter::parse(value.value());
                if (filter.is_error())
                    return vm.throw_completion<JS::TypeError>("Invalid navigation URL filter"_utf16);
                listener.navigation_filter = filter.release_value();
            }
            if (web_request_event.has_value()) {
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
                bool include_headers = false;
                if (vm.argument_count() > 2 && !vm.argument(2).is_undefined()) {
                    auto parsed = JsonValue::from_string(js_value_to_json_string(vm, vm.argument(2)));
                    if (parsed.is_error() || !parsed.value().is_array())
                        return vm.throw_completion<JS::TypeError>("webRequest extraInfoSpec must be an array"_utf16);
                    for (auto const& item : parsed.value().as_array().values()) {
                        auto option = item.is_string() ? item.as_string().bytes_as_string_view() : StringView {};
                        if (option == "blocking"sv && (*web_request_event == Extensions::WebRequestEvent::BeforeRequest || *web_request_event == Extensions::WebRequestEvent::HeadersReceived)) {
                            blocking = true;
                        } else if (option == "requestHeaders"sv && *web_request_event == Extensions::WebRequestEvent::SendHeaders) {
                            include_headers = true;
                        } else if (option == "responseHeaders"sv && *web_request_event == Extensions::WebRequestEvent::HeadersReceived) {
                            include_headers = true;
                        } else if (option != "extraHeaders"sv || *web_request_event != Extensions::WebRequestEvent::SendHeaders) {
                            return vm.throw_completion<JS::TypeError>("Unsupported webRequest extraInfoSpec option"_utf16);
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
                listener.include_headers = include_headers;
                args_obj.set("event"sv, MUST(String::from_utf8(Extensions::to_string(*web_request_event))));
                args_obj.set("includeHeaders"sv, include_headers);
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
        GC::create_function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)>(vm.heap(), GC::captures(GC::Ref { *this }, event_key, web_request_event), [](auto& self, auto const& event_key, auto web_request_event, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
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

            if (web_request_event.has_value()) {
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

void ExtensionRealmContext::notify_runtime_port(StringView method, String const& id, Optional<String> payload)
{
    JsonArray arguments;
    arguments.must_append(id);
    if (payload.has_value())
        arguments.must_append(payload.release_value());
    m_document->page().client().page_did_call_extension_api(s_next_api_call_id.fetch_add(1), m_extension_id, m_context_type,
        m_document->id(), m_grant_generation, "runtime"_string, MUST(String::from_utf8(method)), arguments.serialized());
}

void ExtensionRealmContext::forget_runtime_port(String const& id)
{
    m_runtime_ports.remove(id);
    m_event_listeners.remove(MUST(String::formatted("runtimePortMessage.{}", id)));
    m_event_listeners.remove(MUST(String::formatted("runtimePortDisconnect.{}", id)));
}

JS::Value ExtensionRealmContext::create_runtime_port(String const& id, String const& name, JS::Value sender)
{
    auto& vm = m_realm->vm();
    auto port = JS::Object::create(*m_realm, m_realm->intrinsics().object_prototype());
    port->define_direct_property("name"_utf16_fly_string, JS::PrimitiveString::create(vm, Utf16String::from_utf8(name)), JS::Attribute::Enumerable);
    port->define_direct_property("sender"_utf16_fly_string, sender, JS::default_attributes);
    port->define_direct_property("onMessage"_utf16_fly_string, create_event_object("runtimePortMessage"sv, id), JS::Attribute::Enumerable);
    port->define_direct_property("onDisconnect"_utf16_fly_string, create_event_object("runtimePortDisconnect"sv, id), JS::Attribute::Enumerable);
    auto post_message = JS::NativeFunction::create(*m_realm,
        GC::create_function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)>(vm.heap(), GC::captures(GC::Ref { *this }, id), [](auto& self, auto const& id, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (!self->m_runtime_ports.contains(id))
                return vm.throw_completion<JS::TypeError>("Extension port is disconnected"_utf16);
            auto encoded = serialize_extension_message(vm, vm.argument(0));
            if (encoded.is_error() || encoded.value().byte_count() > 4 * MiB)
                return vm.throw_completion<JS::TypeError>("Unable to clone the extension port message"_utf16);
            self->notify_runtime_port("postPortMessage"sv, id, encoded.release_value());
            return JS::js_undefined();
        }),
        1, "postMessage"_utf16_fly_string, m_realm);
    auto disconnect = JS::NativeFunction::create(*m_realm,
        GC::create_function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)>(vm.heap(), GC::captures(GC::Ref { *this }, id), [](auto& self, auto const& id, JS::VM&) -> JS::ThrowCompletionOr<JS::Value> {
            if (self->m_runtime_ports.contains(id)) {
                self->forget_runtime_port(id);
                self->notify_runtime_port("disconnectPort"sv, id);
            }
            return JS::js_undefined();
        }),
        0, "disconnect"_utf16_fly_string, m_realm);
    port->define_direct_property("postMessage"_utf16_fly_string, post_message, JS::Attribute::Enumerable);
    port->define_direct_property("disconnect"_utf16_fly_string, disconnect, JS::Attribute::Enumerable);
    m_runtime_ports.set(id, port);
    return port;
}

JS::ThrowCompletionOr<JS::Value> ExtensionRealmContext::connect_runtime_port(JS::VM& vm)
{
    if (m_revoked || m_runtime_ports.size() >= 128)
        return vm.throw_completion<JS::TypeError>("Extension port is unavailable"_utf16);
    size_t options_index = 0;
    if (vm.argument(0).is_string()) {
        if (vm.argument(0).as_string().utf16_string().to_utf8() != m_extension_id)
            return vm.throw_completion<JS::TypeError>("Only connections within this extension are supported"_utf16);
        options_index = 1;
    }
    if (vm.argument_count() > options_index + 1)
        return vm.throw_completion<JS::TypeError>("Expected optional extension port options"_utf16);
    auto options = vm.argument(options_index);
    String name;
    if (!options.is_nullish()) {
        if (!options.is_object())
            return vm.throw_completion<JS::TypeError>("Extension port options must be an object"_utf16);
        auto requested_name = TRY(options.as_object().get("name"_utf16_fly_string));
        if (!requested_name.is_undefined()) {
            if (!requested_name.is_string())
                return vm.throw_completion<JS::TypeError>("Extension port name must be a string"_utf16);
            name = requested_name.as_string().utf16_string().to_utf8();
            if (name.byte_count() > 1024)
                return vm.throw_completion<JS::TypeError>("Extension port name is too long"_utf16);
        }
    }
    auto id = generate_random_uuid();
    auto port = create_runtime_port(id, name, JS::js_undefined());
    notify_runtime_port("connectPort"sv, id, name);
    return port;
}

JS::ThrowCompletionOr<JS::Value> ExtensionRealmContext::connect_native_port(JS::VM& vm)
{
    if (m_revoked || m_runtime_ports.size() >= 128)
        return vm.throw_completion<JS::TypeError>("Extension port is unavailable"_utf16);
    if (!vm.argument(0).is_string())
        return vm.throw_completion<JS::TypeError>("runtime.connectNative requires an application name"_utf16);
    auto application = vm.argument(0).as_string().utf16_string().to_utf8();
    if (application.byte_count() > 1024)
        return vm.throw_completion<JS::TypeError>("Native application name is too long"_utf16);
    auto id = generate_random_uuid();
    auto port = create_runtime_port(id, application, JS::js_undefined());
    notify_runtime_port("connectNativePort"sv, id, application);
    return port;
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
    bool creates_menu = ns_name == "menus"sv && method_name == "create"sv;
    if ((is_chrome || creates_menu) && arg_count > 0 && vm.argument(arg_count - 1).is_function()) {
        callback = &vm.argument(arg_count - 1).as_function();
        --arg_count;
    }

    if (creates_menu || (ns_name == "menus"sv && method_name == "update"sv)) {
        size_t properties_index = creates_menu ? 0 : 1;
        if (arg_count != properties_index + 1 || !vm.argument(properties_index).is_object())
            return vm.throw_completion<JS::TypeError>("Expected menu properties"_utf16);
        if (!TRY(vm.argument(properties_index).as_object().get("onclick"_utf16_fly_string)).is_undefined())
            return vm.throw_completion<JS::TypeError>("Use menus.onClicked for page menu items"_utf16);
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

    JS::Value created_menu_id;
    if (creates_menu) {
        auto args = JsonValue::from_string(serialized_args);
        auto const& details = args.value().as_array().at(0);
        if (!details.is_object() || !details.as_object().get_string("id"sv).has_value())
            return vm.throw_completion<JS::TypeError>("Page menu items require a string ID"_utf16);
        created_menu_id = JS::PrimitiveString::create(vm, Utf16String::from_utf8(*details.as_object().get_string("id"sv)));
    }
    u64 call_id = s_next_api_call_id.fetch_add(1);
    m_pending_calls.set(call_id, PendingCall {
                                     .promise = promise,
                                     .callback = callback,
                                     .is_chrome = is_chrome,
                                     .clone_response = message_index.has_value(),
                                     .creates_menu = creates_menu,
                                     .registers_legacy_content_script = ns_name == "contentScripts"sv && method_name == "register"sv,
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

    if (creates_menu)
        return created_menu_id;
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

JS::ThrowCompletionOr<JS::Value> ExtensionRealmContext::answer_locally(JS::VM& vm, bool is_chrome, StringView result_json)
{
    auto value = json_value_to_js_value(vm, *m_realm, result_json);
    auto argument_count = vm.argument_count();
    if (is_chrome && argument_count > 0 && vm.argument(argument_count - 1).is_function()) {
        TRY(JS::call(vm, vm.argument(argument_count - 1).as_function(), JS::js_undefined(), value));
        return JS::js_undefined();
    }
    auto promise = WebIDL::create_promise(*m_realm);
    WebIDL::resolve_promise(*m_realm, *promise, value);
    return promise->promise();
}

void ExtensionRealmContext::define_local_method(JS::Object& object, StringView name, bool is_chrome, LocalAnswer answer)
{
    auto& vm = main_thread_vm();
    auto function = JS::NativeFunction::create(
        *m_realm,
        GC::create_function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)>(vm.heap(), GC::captures(GC::Ref { *this }, is_chrome, answer), [](auto& self, bool is_chrome, auto answer, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            return self->answer_locally(vm, is_chrome, answer(*self));
        }),
        0, Utf16FlyString::from_utf8(name), m_realm);
    object.define_direct_property(Utf16FlyString::from_utf8(name), function, JS::default_attributes);
}

static void define_string_enum(JS::VM& vm, JS::Realm& realm, JS::Object& object, StringView name, ReadonlySpan<StringView> values)
{
    auto values_object = JS::Object::create(realm, realm.intrinsics().object_prototype());
    for (auto value : values) {
        auto key = value.to_byte_string().to_uppercase().replace("-"sv, "_"sv, ReplaceMode::All);
        values_object->define_direct_property(Utf16FlyString::from_utf8(key), JS::PrimitiveString::create(vm, Utf16String::from_utf8(value)), JS::Attribute::Enumerable);
    }
    object.define_direct_property(Utf16FlyString::from_utf8(name), values_object, JS::default_attributes);
}

// What runtime.getPlatformInfo() reports, with Firefox's names for each operating system and processor.
static String platform_info_json()
{
#if defined(AK_OS_MACOS)
    auto os = "mac"sv;
#elif defined(AK_OS_WINDOWS)
    auto os = "win"sv;
#elif defined(AK_OS_ANDROID)
    auto os = "android"sv;
#elif defined(AK_OS_OPENBSD)
    auto os = "openbsd"sv;
#else
    auto os = "linux"sv;
#endif
#if ARCH(AARCH64)
    auto arch = "aarch64"sv;
#elif ARCH(X86_64)
    auto arch = "x86-64"sv;
#elif ARCH(I386)
    auto arch = "x86-32"sv;
#else
    auto arch = "noarch"sv;
#endif
    JsonObject info;
    info.set("os"sv, os);
    info.set("arch"sv, arch);
    return info.serialized();
}

static String browser_info_json()
{
    JsonObject info;
    info.set("name"sv, Extensions::SupportMatrix::browser_name);
    info.set("vendor"sv, Extensions::SupportMatrix::browser_vendor);
    info.set("version"sv, Extensions::SupportMatrix::firefox_api_version);
    info.set("buildID"sv, Extensions::SupportMatrix::browser_build_id);
    return info.serialized();
}

static void append_localized_text(StringBuilder& output, StringView text, Vector<String> const& substitutions, Optional<JsonObject const&> placeholders)
{
    for (size_t i = 0; i < text.length(); ++i) {
        if (text[i] != '$' || i + 1 == text.length()) {
            output.append(text[i]);
            continue;
        }
        char next = text[i + 1];
        if (next == '$') {
            output.append('$');
            ++i;
            continue;
        }
        if (next >= '1' && next <= '9') {
            size_t index = next - '1';
            if (index < substitutions.size())
                output.append(substitutions[index]);
            ++i;
            continue;
        }
        if (placeholders.has_value()) {
            if (auto end = text.find('$', i + 1); end.has_value()) {
                auto name = text.substring_view(i + 1, *end - i - 1);
                Optional<String const&> content;
                placeholders->for_each_member([&](String const& key, JsonValue const& value) {
                    if (!content.has_value() && key.bytes_as_string_view().equals_ignoring_ascii_case(name) && value.is_object())
                        content = value.as_object().get_string("content"sv);
                });
                if (content.has_value()) {
                    append_localized_text(output, *content, substitutions, {});
                    i = *end;
                    continue;
                }
            }
        }
        output.append('$');
    }
}

JS::Value ExtensionRealmContext::create_i18n_object()
{
    auto& vm = m_realm->vm();
    auto object = JS::Object::create(*m_realm, m_realm->intrinsics().object_prototype());
    auto get_message = JS::NativeFunction::create(
        *m_realm,
        GC::create_function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)>(vm.heap(), GC::captures(GC::Ref { *this }), [](auto& self, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            auto name = TRY(vm.argument(0).to_utf16_string(vm)).to_utf8().to_ascii_lowercase();
            auto entry = self->m_locale_messages.get_object(name);
            if (!entry.has_value())
                return JS::PrimitiveString::create(vm, Utf16String {});
            auto text = entry->get_string("message"sv);
            if (!text.has_value())
                return JS::PrimitiveString::create(vm, Utf16String {});

            Vector<String> substitutions;
            auto argument = vm.argument(1);
            if (argument.is_object() && is<JS::Array>(argument.as_object())) {
                auto const& array = argument.as_object();
                auto size = min(TRY(JS::length_of_array_like(vm, array)), 9uz);
                for (u32 i = 0; i < size; ++i) {
                    auto value = TRY(array.get(JS::PropertyKey { i }));
                    substitutions.append(TRY(value.to_utf16_string(vm)).to_utf8());
                }
            } else if (!argument.is_undefined()) {
                substitutions.append(TRY(argument.to_utf16_string(vm)).to_utf8());
            }
            StringBuilder output;
            append_localized_text(output, *text, substitutions, entry->get_object("placeholders"sv));
            return JS::PrimitiveString::create(vm, Utf16String::from_utf8(MUST(output.to_string())));
        }),
        1, "getMessage"_utf16_fly_string, m_realm);
    object->define_direct_property("getMessage"_utf16_fly_string, get_message, JS::default_attributes);

    auto get_ui_language = JS::NativeFunction::create(
        *m_realm,
        GC::create_function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)>(vm.heap(), GC::captures(GC::Ref { *this }), [](auto& self, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            auto locale_entry = self->m_locale_messages.get_object("@@ui_locale"sv);
            auto locale = locale_entry.has_value() ? locale_entry->get_string("message"sv) : Optional<String const&> {};
            auto language = locale.has_value() ? MUST(locale->replace("_"sv, "-"sv, ReplaceMode::All)) : "en"_string;
            return JS::PrimitiveString::create(vm, Utf16String::from_utf8(language));
        }),
        0, "getUILanguage"_utf16_fly_string, m_realm);
    object->define_direct_property("getUILanguage"_utf16_fly_string, get_ui_language, JS::default_attributes);
    return object;
}

void ExtensionRealmContext::install_extension_namespaces(bool full_extension_page_apis)
{
    auto& vm = main_thread_vm();
    auto& global = m_realm->global_object();

    auto parsed_manifest = JsonValue::from_string(m_manifest_json);
    auto manifest = parsed_manifest.is_error() ? JsonValue {} : parsed_manifest.release_value();

    auto build_root = [&](bool is_chrome) -> JS::Value {
        auto root = JS::Object::create(*m_realm, m_realm->intrinsics().object_prototype());

        // runtime
        static constexpr Array content_runtime_methods { "sendMessage"sv };
        static constexpr Array page_runtime_methods { "sendMessage"sv, "openOptionsPage"sv, "setUninstallURL"sv, "reload"sv };
        auto runtime_obj = create_namespace_object("runtime"sv, full_extension_page_apis ? page_runtime_methods.span() : content_runtime_methods.span(), is_chrome);
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
        runtime_js_obj.define_direct_property("onConnect"_utf16_fly_string, create_event_object("runtime"sv, "onConnect"sv), JS::default_attributes);
        auto connect = JS::NativeFunction::create(*m_realm,
            GC::create_function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)>(vm.heap(), GC::captures(GC::Ref { *this }), [](auto& self, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
                return self->connect_runtime_port(vm);
            }),
            1, "connect"_utf16_fly_string, m_realm);
        runtime_js_obj.define_direct_property("connect"_utf16_fly_string, connect, JS::default_attributes);
        if (full_extension_page_apis)
            runtime_js_obj.define_direct_property("onUpdateAvailable"_utf16_fly_string, create_event_object("runtime"sv, "onUpdateAvailable"sv), JS::default_attributes);

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

        // Storage access is checked in the browser against each document's current grant.
        auto storage_obj = JS::Object::create(*m_realm, m_realm->intrinsics().object_prototype());
        static constexpr Array storage_area_methods { "get"sv, "getKeys"sv, "set"sv, "remove"sv, "clear"sv, "getBytesInUse"sv };
        storage_obj->define_direct_property("local"_utf16_fly_string, create_namespace_object("storage.local"sv, storage_area_methods.span(), is_chrome), JS::default_attributes);
        static constexpr Array privileged_session_methods { "get"sv, "getKeys"sv, "set"sv, "remove"sv, "clear"sv, "getBytesInUse"sv, "setAccessLevel"sv };
        auto session_methods = full_extension_page_apis ? privileged_session_methods.span() : storage_area_methods.span();
        auto session = create_namespace_object("storage.session"sv, session_methods, is_chrome);
        session.as_object().define_direct_property("QUOTA_BYTES"_utf16_fly_string, JS::Value(10 * MiB), JS::Attribute::Enumerable);
        storage_obj->define_direct_property("session"_utf16_fly_string, session, JS::default_attributes);
        // Without a sync service, storage.sync keeps its data on this device under Firefox's sync quotas.
        auto sync = create_namespace_object("storage.sync"sv, storage_area_methods.span(), is_chrome);
        sync.as_object().define_direct_property("QUOTA_BYTES"_utf16_fly_string, JS::Value(102400), JS::Attribute::Enumerable);
        sync.as_object().define_direct_property("QUOTA_BYTES_PER_ITEM"_utf16_fly_string, JS::Value(8192), JS::Attribute::Enumerable);
        sync.as_object().define_direct_property("MAX_ITEMS"_utf16_fly_string, JS::Value(512), JS::Attribute::Enumerable);
        sync.as_object().define_direct_property("MAX_WRITE_OPERATIONS_PER_HOUR"_utf16_fly_string, JS::Value(1800), JS::Attribute::Enumerable);
        sync.as_object().define_direct_property("MAX_WRITE_OPERATIONS_PER_MINUTE"_utf16_fly_string, JS::Value(120), JS::Attribute::Enumerable);
        storage_obj->define_direct_property("sync"_utf16_fly_string, sync, JS::default_attributes);
        static constexpr Array managed_methods { "get"sv, "getKeys"sv, "getBytesInUse"sv };
        auto managed = create_namespace_object("storage.managed"sv, managed_methods.span(), is_chrome);
        storage_obj->define_direct_property("managed"_utf16_fly_string, managed, JS::default_attributes);
        for (auto area : Array { "local"sv, "session"sv, "sync"sv, "managed"sv }) {
            auto area_object = MUST(storage_obj->get(Utf16FlyString::from_utf8(area)));
            area_object.as_object().define_direct_property("onChanged"_utf16_fly_string, create_event_object(MUST(String::formatted("storage.{}", area)), "onChanged"sv), JS::default_attributes);
        }
        storage_obj->define_direct_property("onChanged"_utf16_fly_string, create_event_object("storage"sv, "onChanged"sv), JS::default_attributes);
        root->define_direct_property("storage"_utf16_fly_string, storage_obj, JS::default_attributes);

        root->define_direct_property("i18n"_utf16_fly_string, create_i18n_object(), JS::default_attributes);

        // extension: the parts Firefox keeps besides runtime. Extensions never run in private windows here.
        auto extension_obj = JS::Object::create(*m_realm, m_realm->intrinsics().object_prototype());
        extension_obj->define_direct_property("getURL"_utf16_fly_string, get_url_fn, JS::default_attributes);
        extension_obj->define_direct_property("inIncognitoContext"_utf16_fly_string, JS::Value(false), JS::default_attributes);
        root->define_direct_property("extension"_utf16_fly_string, extension_obj, JS::default_attributes);

        if (!full_extension_page_apis)
            return root;

        define_local_method(*extension_obj, "isAllowedIncognitoAccess"sv, is_chrome, [](ExtensionRealmContext&) { return "false"_string; });
        define_local_method(*extension_obj, "isAllowedFileSchemeAccess"sv, is_chrome, [](ExtensionRealmContext&) { return "false"_string; });
        static constexpr Array view_types { "tab"sv, "popup"sv, "sidebar"sv };
        define_string_enum(vm, *m_realm, *extension_obj, "ViewType"sv, view_types.span());

        if (manifest_has_permission(manifest, "nativeMessaging"sv)) {
            auto connect_native = JS::NativeFunction::create(*m_realm,
                GC::create_function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)>(vm.heap(), GC::captures(GC::Ref { *this }), [](auto& self, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
                    return self->connect_native_port(vm);
                }),
                1, "connectNative"_utf16_fly_string, m_realm);
            runtime_js_obj.define_direct_property("connectNative"_utf16_fly_string, connect_native, JS::default_attributes);
            static constexpr Array native_methods { "sendNativeMessage"sv };
            auto native = create_namespace_object("runtime"sv, native_methods.span(), is_chrome);
            runtime_js_obj.define_direct_property("sendNativeMessage"_utf16_fly_string, MUST(native.as_object().get("sendNativeMessage"_utf16_fly_string)), JS::default_attributes);
        }

        if (manifest_has_permission(manifest, "notifications"sv)) {
            static constexpr Array notification_methods { "create"sv, "clear"sv, "getAll"sv };
            auto notifications = create_namespace_object("notifications"sv, notification_methods.span(), is_chrome);
            for (auto event : Array { "onShown"sv, "onClicked"sv, "onClosed"sv, "onButtonClicked"sv })
                notifications.as_object().define_direct_property(Utf16FlyString::from_utf8(event), create_event_object("notifications"sv, event), JS::default_attributes);
            static constexpr Array template_types { "basic"sv, "image"sv, "list"sv, "progress"sv };
            define_string_enum(vm, *m_realm, notifications.as_object(), "TemplateType"sv, template_types.span());
            root->define_direct_property("notifications"_utf16_fly_string, notifications, JS::default_attributes);
        }

        if (manifest_has_permission(manifest, "downloads"sv)) {
            static constexpr Array download_methods { "download"sv, "search"sv, "cancel"sv, "pause"sv, "resume"sv };
            auto downloads = create_namespace_object("downloads"sv, download_methods.span(), is_chrome);
            for (auto event : Array { "onCreated"sv, "onChanged"sv, "onErased"sv })
                downloads.as_object().define_direct_property(Utf16FlyString::from_utf8(event), create_event_object("downloads"sv, event), JS::default_attributes);
            static constexpr Array states { "in_progress"sv, "interrupted"sv, "complete"sv };
            static constexpr Array conflict_actions { "uniquify"sv, "overwrite"sv, "prompt"sv };
            define_string_enum(vm, *m_realm, downloads.as_object(), "State"sv, states.span());
            define_string_enum(vm, *m_realm, downloads.as_object(), "FilenameConflictAction"sv, conflict_actions.span());
            root->define_direct_property("downloads"_utf16_fly_string, downloads, JS::default_attributes);
        }

        if (manifest_has_permission(manifest, "cookies"sv)) {
            static constexpr Array cookie_methods { "get"sv, "getAll"sv, "set"sv, "remove"sv, "getAllCookieStores"sv };
            auto cookies = create_namespace_object("cookies"sv, cookie_methods.span(), is_chrome);
            cookies.as_object().define_direct_property("onChanged"_utf16_fly_string, create_event_object("cookies"sv, "onChanged"sv), JS::default_attributes);
            static constexpr Array same_site_values { "no_restriction"sv, "lax"sv, "strict"sv };
            static constexpr Array change_causes { "evicted"sv, "expired"sv, "explicit"sv, "expired_overwrite"sv, "overwrite"sv };
            define_string_enum(vm, *m_realm, cookies.as_object(), "SameSiteStatus"sv, same_site_values.span());
            define_string_enum(vm, *m_realm, cookies.as_object(), "OnChangedCause"sv, change_causes.span());
            root->define_direct_property("cookies"_utf16_fly_string, cookies, JS::default_attributes);
        }

        if (manifest_has_permission(manifest, "idle"sv)) {
            static constexpr Array idle_methods { "queryState"sv, "setDetectionInterval"sv };
            auto idle = create_namespace_object("idle"sv, idle_methods.span(), is_chrome);
            idle.as_object().define_direct_property("onStateChanged"_utf16_fly_string, create_event_object("idle"sv, "onStateChanged"sv), JS::default_attributes);
            static constexpr Array idle_states { "active"sv, "idle"sv, "locked"sv };
            define_string_enum(vm, *m_realm, idle.as_object(), "IdleState"sv, idle_states.span());
            root->define_direct_property("idle"_utf16_fly_string, idle, JS::default_attributes);
        }

        define_local_method(runtime_js_obj, "getPlatformInfo"sv, is_chrome, [](ExtensionRealmContext&) { return platform_info_json(); });
        define_local_method(runtime_js_obj, "getBrowserInfo"sv, is_chrome, [](ExtensionRealmContext&) { return browser_info_json(); });
        for (auto event : Array { "onStartup"sv, "onSuspend"sv, "onSuspendCanceled"sv, "onMessageExternal"sv, "onConnectExternal"sv })
            runtime_js_obj.define_direct_property(Utf16FlyString::from_utf8(event), create_event_object("runtime"sv, event), JS::default_attributes);
        static constexpr Array platform_os { "mac"sv, "win"sv, "android"sv, "cros"sv, "linux"sv, "openbsd"sv, "fuchsia"sv };
        static constexpr Array platform_arch { "aarch64"sv, "arm"sv, "ppc64"sv, "s390x"sv, "sparc64"sv, "x86-32"sv, "x86-64"sv, "noarch"sv };
        static constexpr Array installed_reasons { "install"sv, "update"sv, "browser_update"sv };
        static constexpr Array restart_reasons { "app_update"sv, "os_update"sv, "periodic"sv };
        define_string_enum(vm, *m_realm, runtime_js_obj, "PlatformOs"sv, platform_os.span());
        define_string_enum(vm, *m_realm, runtime_js_obj, "PlatformArch"sv, platform_arch.span());
        define_string_enum(vm, *m_realm, runtime_js_obj, "OnInstalledReason"sv, installed_reasons.span());
        define_string_enum(vm, *m_realm, runtime_js_obj, "OnRestartRequiredReason"sv, restart_reasons.span());

        static constexpr Array command_methods { "getAll"sv };
        auto commands = create_namespace_object("commands"sv, command_methods.span(), is_chrome);
        commands.as_object().define_direct_property("onCommand"_utf16_fly_string, create_event_object("commands"sv, "onCommand"sv), JS::default_attributes);
        root->define_direct_property("commands"_utf16_fly_string, commands, JS::default_attributes);

        if (manifest_has_permission(manifest, "alarms"sv)) {
            static constexpr Array alarms_methods { "create"sv, "get"sv, "getAll"sv, "clear"sv, "clearAll"sv };
            auto alarms_obj = create_namespace_object("alarms"sv, alarms_methods.span(), is_chrome);
            alarms_obj.as_object().define_direct_property("onAlarm"_utf16_fly_string, create_event_object("alarms"sv, "onAlarm"sv), JS::default_attributes);
            root->define_direct_property("alarms"_utf16_fly_string, alarms_obj, JS::default_attributes);
        }

        if (manifest_has_permission(manifest, "publicSuffix"sv))
            root->define_direct_property("publicSuffix"_utf16_fly_string, make_public_suffix_api(*m_realm), JS::default_attributes);

        if (manifest_has_permission(manifest, "dns"sv)) {
            static constexpr Array dns_methods { "resolve"sv };
            root->define_direct_property("dns"_utf16_fly_string, create_namespace_object("dns"sv, dns_methods.span(), is_chrome), JS::default_attributes);
        }

        if (manifest_has_permission(manifest, "declarativeNetRequest"sv) || manifest_has_permission(manifest, "declarativeNetRequestWithHostAccess"sv)) {
            static constexpr Array request_methods { "getEnabledRulesets"sv, "updateEnabledRulesets"sv, "getAvailableStaticRuleCount"sv,
                "getDynamicRules"sv, "updateDynamicRules"sv, "getSessionRules"sv, "updateSessionRules"sv, "isRegexSupported"sv };
            auto declarative_api = create_namespace_object("declarativeNetRequest"sv, request_methods.span(), is_chrome);
            declarative_api.as_object().define_direct_property("MAX_NUMBER_OF_REGEX_RULES"_utf16_fly_string, JS::Value(0), JS::Attribute::Enumerable);
            root->define_direct_property("declarativeNetRequest"_utf16_fly_string, declarative_api, JS::default_attributes);
        }

        if (manifest_has_permission(manifest, "privacy"sv)) {
            auto privacy = JS::Object::create(*m_realm, m_realm->intrinsics().object_prototype());
            auto network = JS::Object::create(*m_realm, m_realm->intrinsics().object_prototype());
            static constexpr Array setting_methods { "get"sv, "set"sv, "clear"sv };
            auto prediction = create_namespace_object("privacy.network.networkPredictionEnabled"sv, setting_methods.span(), is_chrome);
            prediction.as_object().define_direct_property("onChange"_utf16_fly_string, create_event_object("privacy.network.networkPredictionEnabled"sv, "onChange"sv), JS::default_attributes);
            auto global_privacy_control = create_namespace_object("privacy.network.globalPrivacyControl"sv, setting_methods.span(), is_chrome);
            global_privacy_control.as_object().define_direct_property("onChange"_utf16_fly_string, create_event_object("privacy.network.globalPrivacyControl"sv, "onChange"sv), JS::default_attributes);
            network->define_direct_property("networkPredictionEnabled"_utf16_fly_string, prediction, JS::default_attributes);
            network->define_direct_property("globalPrivacyControl"_utf16_fly_string, global_privacy_control, JS::default_attributes);
            privacy->define_direct_property("network"_utf16_fly_string, network, JS::default_attributes);
            auto websites = JS::Object::create(*m_realm, m_realm->intrinsics().object_prototype());
            websites->define_direct_property("hyperlinkAuditingEnabled"_utf16_fly_string, create_namespace_object("privacy.websites.hyperlinkAuditingEnabled"sv, setting_methods.span(), is_chrome), JS::default_attributes);
            privacy->define_direct_property("websites"_utf16_fly_string, websites, JS::default_attributes);
            root->define_direct_property("privacy"_utf16_fly_string, privacy, JS::default_attributes);
        }

        // tabs
        static constexpr Array tabs_methods { "query"sv, "get"sv, "getCurrent"sv, "create"sv, "update"sv, "reload"sv, "remove"sv, "sendMessage"sv, "executeScript"sv, "insertCSS"sv, "removeCSS"sv };
        auto tabs_obj = create_namespace_object("tabs"sv, tabs_methods.span(), is_chrome);
        auto& tabs_js_obj = tabs_obj.as_object();
        tabs_js_obj.define_direct_property("TAB_ID_NONE"_utf16_fly_string, JS::Value(-1), JS::Attribute::Enumerable);
        tabs_js_obj.define_direct_property("onUpdated"_utf16_fly_string, create_event_object("tabs"sv, "onUpdated"sv), JS::default_attributes);
        tabs_js_obj.define_direct_property("onActivated"_utf16_fly_string, create_event_object("tabs"sv, "onActivated"sv), JS::default_attributes);
        tabs_js_obj.define_direct_property("onCreated"_utf16_fly_string, create_event_object("tabs"sv, "onCreated"sv), JS::default_attributes);
        tabs_js_obj.define_direct_property("onRemoved"_utf16_fly_string, create_event_object("tabs"sv, "onRemoved"sv), JS::default_attributes);
        // Ladyflock never swaps one tab's page for another's, as prerendering does, so onReplaced has nothing to report.
        tabs_js_obj.define_direct_property("onReplaced"_utf16_fly_string, create_event_object("tabs"sv, "onReplaced"sv), JS::default_attributes);
        root->define_direct_property("tabs"_utf16_fly_string, tabs_obj, JS::default_attributes);

        if (manifest_has_permission(manifest, "webNavigation"sv)) {
            static constexpr Array navigation_methods { "getFrame"sv, "getAllFrames"sv };
            auto navigation = create_namespace_object("webNavigation"sv, navigation_methods.span(), is_chrome);
            for (auto name : Array { "onBeforeNavigate"sv, "onCommitted"sv, "onCompleted"sv, "onDOMContentLoaded"sv, "onHistoryStateUpdated"sv, "onReferenceFragmentUpdated"sv, "onErrorOccurred"sv, "onCreatedNavigationTarget"sv })
                navigation.as_object().define_direct_property(Utf16FlyString::from_utf8(name), create_event_object("webNavigation"sv, name), JS::default_attributes);
            root->define_direct_property("webNavigation"_utf16_fly_string, navigation, JS::default_attributes);
        }

        static constexpr Array window_methods { "getCurrent"sv, "getLastFocused"sv, "get"sv, "getAll"sv };
        auto windows = create_namespace_object("windows"sv, window_methods.span(), is_chrome);
        windows.as_object().define_direct_property("onFocusChanged"_utf16_fly_string, create_event_object("windows"sv, "onFocusChanged"sv), JS::default_attributes);
        windows.as_object().define_direct_property("WINDOW_ID_NONE"_utf16_fly_string, JS::Value(-1), JS::default_attributes);
        windows.as_object().define_direct_property("WINDOW_ID_CURRENT"_utf16_fly_string, JS::Value(-2), JS::default_attributes);
        static constexpr Array window_types { "normal"sv, "popup"sv, "panel"sv, "devtools"sv };
        static constexpr Array window_states { "normal"sv, "minimized"sv, "maximized"sv, "fullscreen"sv, "docked"sv };
        define_string_enum(vm, *m_realm, windows.as_object(), "WindowType"sv, window_types.span());
        define_string_enum(vm, *m_realm, windows.as_object(), "WindowState"sv, window_states.span());

        // management: an extension may always describe or remove itself; the rest needs the management permission.
        static constexpr Array self_management_methods { "getSelf"sv, "uninstallSelf"sv };
        static constexpr Array management_methods { "getSelf"sv, "uninstallSelf"sv, "getAll"sv, "get"sv };
        bool can_manage = manifest_has_permission(manifest, "management"sv);
        auto management = create_namespace_object("management"sv, can_manage ? management_methods.span() : self_management_methods.span(), is_chrome);
        if (can_manage) {
            for (auto event : Array { "onInstalled"sv, "onUninstalled"sv, "onEnabled"sv, "onDisabled"sv })
                management.as_object().define_direct_property(Utf16FlyString::from_utf8(event), create_event_object("management"sv, event), JS::default_attributes);
        }
        root->define_direct_property("management"_utf16_fly_string, management, JS::default_attributes);
        root->define_direct_property("windows"_utf16_fly_string, windows, JS::default_attributes);

        // scripting
        static constexpr Array scripting_methods { "executeScript"sv, "insertCSS"sv, "removeCSS"sv, "registerContentScripts"sv, "getRegisteredContentScripts"sv, "updateContentScripts"sv, "unregisterContentScripts"sv };
        root->define_direct_property("scripting"_utf16_fly_string, create_namespace_object("scripting"sv, scripting_methods.span(), is_chrome), JS::default_attributes);
        if (manifest.is_object() && manifest.as_object().get_u32("manifest_version"sv) == 2u) {
            static constexpr Array content_script_methods { "register"sv };
            root->define_direct_property("contentScripts"_utf16_fly_string, create_namespace_object("contentScripts"sv, content_script_methods.span(), is_chrome), JS::default_attributes);
        }

        // action + browserAction
        static constexpr Array action_methods { "setTitle"sv, "getTitle"sv, "setBadgeText"sv, "getBadgeText"sv, "setBadgeBackgroundColor"sv, "getBadgeBackgroundColor"sv, "setPopup"sv, "getPopup"sv, "setIcon"sv, "enable"sv, "disable"sv };
        auto action_obj = create_namespace_object("action"sv, action_methods.span(), is_chrome);
        action_obj.as_object().define_direct_property("onClicked"_utf16_fly_string, create_event_object("action"sv, "onClicked"sv), JS::default_attributes);
        root->define_direct_property("action"_utf16_fly_string, action_obj, JS::default_attributes);

        auto browser_action_obj = create_namespace_object("browserAction"sv, action_methods.span(), is_chrome);
        browser_action_obj.as_object().define_direct_property("onClicked"_utf16_fly_string, create_event_object("browserAction"sv, "onClicked"sv), JS::default_attributes);
        root->define_direct_property("browserAction"_utf16_fly_string, browser_action_obj, JS::default_attributes);

        if (manifest_has_permission(manifest, "menus"sv) || manifest_has_permission(manifest, "contextMenus"sv)) {
            static constexpr Array methods { "create"sv, "update"sv, "remove"sv, "removeAll"sv };
            auto menus = create_namespace_object("menus"sv, methods.span(), is_chrome);
            menus.as_object().define_direct_property("onClicked"_utf16_fly_string, create_event_object("menus"sv, "onClicked"sv), JS::default_attributes);
            root->define_direct_property("menus"_utf16_fly_string, menus, JS::default_attributes);
            root->define_direct_property("contextMenus"_utf16_fly_string, menus, JS::default_attributes);
        }

        // sidebarAction
        static constexpr Array sidebar_methods { "setPanel"sv, "getPanel"sv, "setTitle"sv, "getTitle"sv, "setIcon"sv, "open"sv, "close"sv, "toggle"sv, "isOpen"sv };
        root->define_direct_property("sidebarAction"_utf16_fly_string, create_namespace_object("sidebarAction"sv, sidebar_methods.span(), is_chrome), JS::default_attributes);

        // permissions
        static constexpr Array permissions_methods { "contains"sv, "getAll"sv, "remove"sv, "request"sv };
        auto perms_obj = create_namespace_object("permissions"sv, permissions_methods.span(), is_chrome);
        perms_obj.as_object().define_direct_property("onAdded"_utf16_fly_string, create_event_object("permissions"sv, "onAdded"sv), JS::default_attributes);
        perms_obj.as_object().define_direct_property("onRemoved"_utf16_fly_string, create_event_object("permissions"sv, "onRemoved"sv), JS::default_attributes);
        root->define_direct_property("permissions"_utf16_fly_string, perms_obj, JS::default_attributes);

        if (manifest_has_permission(manifest, "webRequest"sv)) {
            static constexpr Array methods { "handlerBehaviorChanged"sv };
            auto web_request = create_namespace_object("webRequest"sv, methods.span(), is_chrome);
            for (auto event : Array { "onBeforeRequest"sv, "onSendHeaders"sv, "onHeadersReceived"sv, "onBeforeRedirect"sv, "onResponseStarted"sv, "onCompleted"sv, "onErrorOccurred"sv })
                web_request.as_object().define_direct_property(Utf16FlyString::from_utf8(event), create_event_object("webRequest"sv, event), JS::default_attributes);

            auto resource_types = JS::Object::create(*m_realm, m_realm->intrinsics().object_prototype());
            for (auto type : Array { "main_frame"sv, "sub_frame"sv, "stylesheet"sv, "script"sv, "image"sv, "font"sv, "object"sv, "xmlhttprequest"sv, "media"sv, "other"sv })
                resource_types->define_direct_property(Utf16FlyString::from_utf8(type.to_byte_string().to_uppercase()), JS::PrimitiveString::create(vm, Utf16String::from_utf8(type)), JS::default_attributes);
            web_request.as_object().define_direct_property("ResourceType"_utf16_fly_string, resource_types, JS::default_attributes);
            root->define_direct_property("webRequest"_utf16_fly_string, web_request, JS::default_attributes);
        }

        return root;
    };

    global.define_direct_property("browser"_utf16_fly_string, build_root(false), JS::default_attributes);
    global.define_direct_property("chrome"_utf16_fly_string, build_root(true), JS::default_attributes);
}

// The browser sends an error as a JSON string; the extension sees its text, as Firefox gives it.
static String api_error_message(StringView error_json)
{
    auto parsed = JsonValue::from_string(error_json);
    if (!parsed.is_error() && parsed.value().is_string())
        return parsed.value().as_string();
    return MUST(String::from_utf8(error_json));
}

void ExtensionRealmContext::handle_api_response(u64 call_id, bool success, StringView result_or_error_json)
{
    auto pending = m_pending_calls.take(call_id);
    if (!pending.has_value() || m_revoked)
        return;

    auto& vm = main_thread_vm();
    HTML::TemporaryExecutionContext entry_context(*m_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
    if (m_execution_context)
        vm.push_execution_context(*m_execution_context);
    ScopeGuard pop_context = [&] {
        if (m_execution_context) {
            auto* popped = vm.pop_execution_context();
            VERIFY(popped == m_execution_context.ptr());
        }
    };

    auto decode_response = [&]() -> JS::Value {
        auto value = json_value_to_js_value(vm, *m_realm, result_or_error_json);
        if (pending->clone_response)
            return deserialize_extension_message(vm, *m_realm, value).value_or(JS::js_undefined());
        if (!pending->registers_legacy_content_script || !value.is_string())
            return value;
        auto id = value.as_string().utf16_string().to_utf8();
        auto handle = JS::Object::create(*m_realm, m_realm->intrinsics().object_prototype());
        auto unregister = JS::NativeFunction::create(*m_realm,
            GC::create_function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)>(vm.heap(), GC::captures(GC::Ref { *this }, move(id)), [](auto& self, auto const& id, JS::VM&) -> JS::ThrowCompletionOr<JS::Value> {
                auto promise = WebIDL::create_promise(*self->m_realm);
                if (self->m_revoked) {
                    WebIDL::reject_promise(*self->m_realm, *promise, JS::Error::create(*self->m_realm, "Extension context invalidated"_utf16));
                    return promise->promise();
                }
                JsonArray arguments;
                arguments.must_append(id);
                auto unregister_call_id = s_next_api_call_id.fetch_add(1);
                self->m_pending_calls.set(unregister_call_id, PendingCall { .promise = promise, .callback = nullptr });
                self->m_document->page().client().page_did_call_extension_api(unregister_call_id, self->m_extension_id, self->m_context_type,
                    self->m_document->id(), self->m_grant_generation, "contentScripts"_string, "unregister"_string, arguments.serialized());
                return promise->promise();
            }),
            0, "unregister"_utf16_fly_string, m_realm);
        handle->define_direct_property("unregister"_utf16_fly_string, unregister, JS::default_attributes);
        return handle;
    };
    if (pending->callback) {
        if (success) {
            m_last_error = {};
            if (pending->creates_menu) {
                (void)JS::call(vm, *pending->callback, JS::js_undefined());
            } else {
                auto arg = decode_response();
                (void)JS::call(vm, *pending->callback, JS::js_undefined(), arg);
            }
        } else {
            m_last_error = api_error_message(result_or_error_json);
            (void)JS::call(vm, *pending->callback, JS::js_undefined());
            m_last_error = {};
        }
        return;
    }

    // menus.create returns its ID synchronously, with completion optionally delivered to a callback.
    if (pending->creates_menu) {
        if (!success) {
            auto error = JS::Error::create(*m_realm, Utf16String::from_utf8(api_error_message(result_or_error_json)));
            HTML::report_exception_to_console(error, *m_realm, HTML::ErrorInPromise::No);
        }
        return;
    }

    if (success) {
        auto val = decode_response();
        WebIDL::resolve_promise(*m_realm, *pending->promise, val);
    } else {
        auto err = JS::Error::create(*m_realm, Utf16String::from_utf8(api_error_message(result_or_error_json)));
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
    Optional<String> runtime_port_id;
    bool port_message = event_key == "runtime.onPortMessage"sv;
    bool port_disconnect = event_key == "runtime.onPortDisconnect"sv;
    if (port_message || port_disconnect) {
        auto arguments = JsonValue::from_string(args_json);
        if (arguments.is_error() || !arguments.value().is_array() || arguments.value().as_array().is_empty() || !arguments.value().as_array().at(0).is_string())
            return;
        runtime_port_id = arguments.value().as_array().at(0).as_string();
        if (!m_runtime_ports.contains(*runtime_port_id))
            return;
        event_key = MUST(String::formatted("{}.{}", port_message ? "runtimePortMessage"sv : "runtimePortDisconnect"sv, *runtime_port_id));
    }
    ScopeGuard cleanup_port = [&] {
        if (port_disconnect && runtime_port_id.has_value())
            forget_runtime_port(*runtime_port_id);
    };
    // Each storage area's own onChanged hears the changes to that area, without the area's name.
    if (event_key == "storage.onChanged"sv) {
        auto arguments = JsonValue::from_string(args_json);
        if (!arguments.is_error() && arguments.value().is_array() && arguments.value().as_array().size() == 2 && arguments.value().as_array().at(1).is_string()) {
            auto area_key = MUST(String::formatted("storage.{}.onChanged", arguments.value().as_array().at(1).as_string()));
            if (auto area_listeners = m_event_listeners.get(area_key); area_listeners.has_value() && !area_listeners->is_empty()) {
                auto listeners = *area_listeners;
                auto& vm = main_thread_vm();
                HTML::TemporaryExecutionContext entry_context(*m_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
                if (m_execution_context)
                    vm.push_execution_context(*m_execution_context);
                ScopeGuard pop_context = [&] {
                    if (m_execution_context) {
                        auto* popped = vm.pop_execution_context();
                        VERIFY(popped == m_execution_context.ptr());
                    }
                };
                auto changes = json_value_to_js_value(vm, *m_realm, arguments.value().as_array().at(0).serialized());
                for (auto const& listener : listeners) {
                    auto result = JS::call(vm, *listener.callback, JS::js_undefined(), changes);
                    if (result.is_throw_completion())
                        HTML::report_exception_to_console(result.release_error().value(), *m_realm, HTML::ErrorInPromise::No);
                }
            }
        }
    }

    auto it = m_event_listeners.find(event_key);
    if (it == m_event_listeners.end() || it->value.is_empty()) {
        if (reply_id.has_value())
            m_document->page().client().page_did_reply_extension_event(*reply_id, false, "null"_string);
        return;
    }

    auto& vm = main_thread_vm();
    HTML::TemporaryExecutionContext entry_context(*m_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
    if (m_execution_context)
        vm.push_execution_context(*m_execution_context);
    ScopeGuard pop_context = [&] {
        if (m_execution_context) {
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

    if (event_key == "runtime.onConnect"sv) {
        if (args.size() != 3 || !args[0].is_string() || !args[1].is_string() || !args[2].is_object() || m_runtime_ports.size() >= 128) {
            if (reply_id.has_value())
                m_document->page().client().page_did_reply_extension_event(*reply_id, false, "null"_string);
            return;
        }
        auto id = args[0].as_string().utf16_string().to_utf8();
        auto name = args[1].as_string().utf16_string().to_utf8();
        auto port = create_runtime_port(id, name, args[2]);
        for (auto const& listener : listeners) {
            auto result = JS::call(vm, *listener.callback, JS::js_undefined(), port);
            if (result.is_throw_completion())
                HTML::report_exception_to_console(result.release_error().value(), *m_realm, HTML::ErrorInPromise::No);
        }
        if (reply_id.has_value())
            m_document->page().client().page_did_reply_extension_event(*reply_id, true, "null"_string);
        return;
    }
    if (runtime_port_id.has_value()) {
        auto port = m_runtime_ports.get(*runtime_port_id).value();
        if (port_disconnect) {
            if (args.size() == 2 && args[1].is_string())
                port->define_direct_property("error"_utf16_fly_string, JS::Error::create(*m_realm, args[1].as_string().utf16_string()), JS::Attribute::Enumerable);
            forget_runtime_port(*runtime_port_id);
            for (auto const& listener : listeners)
                (void)JS::call(vm, *listener.callback, JS::js_undefined(), port);
        } else if (args.size() == 2) {
            auto message = deserialize_extension_message(vm, *m_realm, args[1]);
            if (message.has_value()) {
                for (auto const& listener : listeners)
                    (void)JS::call(vm, *listener.callback, JS::js_undefined(), *message, port);
            }
        }
        return;
    }

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

    if (namespace_name == "webRequest"sv) {
        auto parsed = JsonValue::from_string(args_json);
        if (parsed.is_error() || !parsed.value().is_array() || parsed.value().as_array().is_empty() || !parsed.value().as_array().at(0).is_object()) {
            if (reply_id.has_value())
                m_document->page().client().page_did_reply_extension_event(*reply_id, false, "null"_string);
            return;
        }
        auto const& details = parsed.value().as_array().at(0).as_object();
        auto url = URL::Parser::basic_parse(details.get_string("url"sv).value_or({}));
        auto type = Extensions::web_request_resource_type_from_string(details.get_string("type"sv).value_or({}));

        struct DecisionState : public RefCounted<DecisionState> {
            GC_HOLDS_NO_CELL_EDGES;
            size_t pending { 1 };
            JsonObject decision;
            void complete(ExtensionRealmContext& context, u64 reply_id, JS::Value value)
            {
                if (value.is_object()) {
                    auto cancellation = value.as_object().get("cancel"_utf16_fly_string);
                    if (!cancellation.is_error() && cancellation.value().to_boolean())
                        decision.set("cancel"sv, true);
                    auto redirect = value.as_object().get("redirectUrl"_utf16_fly_string);
                    if (!decision.has("redirectUrl"sv) && !redirect.is_error() && redirect.value().is_string())
                        decision.set("redirectUrl"sv, redirect.value().as_string().utf16_string().to_utf8());
                    auto headers = value.as_object().get("responseHeaders"_utf16_fly_string);
                    if (!decision.has("responseHeaders"sv) && !headers.is_error() && headers.value().is_object()) {
                        auto parsed = JsonValue::from_string(js_value_to_json_string(main_thread_vm(), headers.value()));
                        if (!parsed.is_error() && parsed.value().is_array())
                            decision.set("responseHeaders"sv, parsed.release_value());
                    }
                }
                if (--pending != 0 || context.is_revoked())
                    return;
                context.m_document->page().client().page_did_reply_extension_event(reply_id, true, decision.serialized());
            }
        };
        auto state = make_ref_counted<DecisionState>();

        for (auto const& listener : listeners) {
            if (!url.has_value() || !any_of(listener.urls, [&](auto const& pattern) { return pattern.matches(*url); }))
                continue;
            if (!listener.types.is_empty() && (!type.has_value() || !listener.types.contains_slow(*type)))
                continue;
            auto listener_details = details;
            if (!listener.include_headers) {
                listener_details.remove("requestHeaders"sv);
                listener_details.remove("responseHeaders"sv);
            }
            auto argument = json_value_to_js_value(vm, *m_realm, listener_details.serialized());
            auto result = JS::call(vm, *listener.callback, JS::js_undefined(), argument);
            if (result.is_throw_completion()) {
                HTML::report_exception_to_console(result.release_error().value(), *m_realm, HTML::ErrorInPromise::No);
                continue;
            }
            if (!listener.blocking || !reply_id.has_value())
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

    Optional<URL::URL> navigation_url;
    if (namespace_name == "webNavigation"sv) {
        auto parsed = JsonValue::from_string(args_json);
        if (!parsed.is_error() && parsed.value().is_array() && !parsed.value().as_array().is_empty() && parsed.value().as_array().at(0).is_object())
            navigation_url = URL::Parser::basic_parse(parsed.value().as_array().at(0).as_object().get_string("url"sv).value_or({}));
    }
    for (auto const& listener : listeners) {
        if (listener.navigation_filter && (!navigation_url.has_value() || !listener.navigation_filter->matches(*navigation_url)))
            continue;
        (void)JS::call(vm, *listener.callback, JS::js_undefined(), args.span());
    }

    if (reply_id.has_value())
        m_document->page().client().page_did_reply_extension_event(*reply_id, true, "null"_string);
}

}
