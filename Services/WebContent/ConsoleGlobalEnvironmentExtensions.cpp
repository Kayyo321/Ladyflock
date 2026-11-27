/*
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ConsoleGlobalEnvironmentExtensions.h"
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/ConsoleObject.h>
#include <LibJS/Runtime/HostObject.h>
#include <LibJS/Runtime/JSONObject.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/NodeList.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/WebIDL/ExceptionOrUtils.h>
#include <LibWeb/XPath/XPath.h>
#include <LibWeb/XPath/XPathResult.h>
#include <LibWebCommon/Clipboard/SystemClipboard.h>

namespace WebContent {

GC_DEFINE_ALLOCATOR(ConsoleGlobalEnvironmentExtensions);

static constexpr JSHostClass console_global_environment_extensions_host_class = JS::make_host_class(JS_HOST_CLASS_OBJECT, "ConsoleGlobalEnvironmentExtensions"sv, nullptr, nullptr, nullptr, 0);

GC::Ref<ConsoleGlobalEnvironmentExtensions> ConsoleGlobalEnvironmentExtensions::create(JS::Realm& realm, Web::HTML::Window& window)
{
    auto extensions = realm.create<ConsoleGlobalEnvironmentExtensions>(window);

    auto binding_object = JS::HostObject::create(realm, console_global_environment_extensions_host_class, nullptr, nullptr, extensions);
    binding_object->define_native_accessor(realm, "$0"_utf16_fly_string, $0_getter, nullptr, 0);
    binding_object->define_native_accessor(realm, "$1"_utf16_fly_string, $1_getter, nullptr, 0);
    binding_object->define_native_accessor(realm, "$2"_utf16_fly_string, $2_getter, nullptr, 0);
    binding_object->define_native_accessor(realm, "$3"_utf16_fly_string, $3_getter, nullptr, 0);
    binding_object->define_native_accessor(realm, "$4"_utf16_fly_string, $4_getter, nullptr, 0);
    binding_object->define_native_accessor(realm, "$_"_utf16_fly_string, $__getter, nullptr, 0);
    binding_object->define_native_function(realm, "$"_utf16_fly_string, $_function, 2, JS::default_attributes);
    binding_object->define_native_function(realm, "$$"_utf16_fly_string, $$_function, 2, JS::default_attributes);
    binding_object->define_native_function(realm, "$x"_utf16_fly_string, $x_function, 2, JS::default_attributes);
    binding_object->define_native_function(realm, "keys"_utf16_fly_string, keys_function, 1, JS::default_attributes);
    binding_object->define_native_function(realm, "values"_utf16_fly_string, values_function, 1, JS::default_attributes);
    binding_object->define_native_function(realm, "dir"_utf16_fly_string, dir_function, 1, JS::default_attributes);
    binding_object->define_native_function(realm, "table"_utf16_fly_string, table_function, 2, JS::default_attributes);
    binding_object->define_native_function(realm, "clear"_utf16_fly_string, clear_function, 0, JS::default_attributes);
    binding_object->define_native_function(realm, "copy"_utf16_fly_string, copy_function, 1, JS::default_attributes);
    binding_object->define_native_function(realm, "inspect"_utf16_fly_string, inspect_function, 1, JS::default_attributes);
    extensions->m_binding_object = binding_object;

    return extensions;
}

ConsoleGlobalEnvironmentExtensions::ConsoleGlobalEnvironmentExtensions(Web::HTML::Window& window)
    : m_window_object(window)
{
}

void ConsoleGlobalEnvironmentExtensions::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_window_object);
    visitor.visit(m_binding_object);
    visitor.visit(m_most_recent_result);
}

static JS::ThrowCompletionOr<ConsoleGlobalEnvironmentExtensions*> get_console(JS::VM& vm)
{
    if (auto this_value = vm.this_value(); this_value.is_object()) {
        if (auto* extensions = JS::host_data_if<ConsoleGlobalEnvironmentExtensions>(this_value.as_object()))
            return extensions;
    }
    return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "ConsoleGlobalEnvironmentExtensions");
}

JS::ThrowCompletionOr<JS::Value> ConsoleGlobalEnvironmentExtensions::get_inspected_node_at(JS::VM& vm, size_t index)
{
    auto* extensions = TRY(get_console(vm));
    auto& realm = extensions->binding_object().shape().realm();
    auto& window = *extensions->m_window_object;
    auto inspected_node = window.associated_document().console_inspected_node(index);
    if (!inspected_node)
        return JS::js_undefined();

    return Web::Bindings::wrap(Web::Bindings::host_defined_wrapper_world(realm), realm, GC::Ref { const_cast<Web::DOM::Node&>(*inspected_node) });
}

JS_DEFINE_NATIVE_FUNCTION(ConsoleGlobalEnvironmentExtensions::$0_getter)
{
    return get_inspected_node_at(vm, 0);
}

JS_DEFINE_NATIVE_FUNCTION(ConsoleGlobalEnvironmentExtensions::$1_getter)
{
    return get_inspected_node_at(vm, 1);
}

JS_DEFINE_NATIVE_FUNCTION(ConsoleGlobalEnvironmentExtensions::$2_getter)
{
    return get_inspected_node_at(vm, 2);
}

JS_DEFINE_NATIVE_FUNCTION(ConsoleGlobalEnvironmentExtensions::$3_getter)
{
    return get_inspected_node_at(vm, 3);
}

JS_DEFINE_NATIVE_FUNCTION(ConsoleGlobalEnvironmentExtensions::$4_getter)
{
    return get_inspected_node_at(vm, 4);
}

JS_DEFINE_NATIVE_FUNCTION(ConsoleGlobalEnvironmentExtensions::$__getter)
{
    auto* extensions = TRY(get_console(vm));
    return extensions->m_most_recent_result;
}

JS_DEFINE_NATIVE_FUNCTION(ConsoleGlobalEnvironmentExtensions::$_function)
{
    auto* extensions = TRY(get_console(vm));
    auto& realm = extensions->binding_object().shape().realm();
    auto& window = *extensions->m_window_object;

    auto selector = TRY(vm.argument(0).to_utf16_string(vm));

    if (vm.argument_count() > 1) {
        auto node = vm.argument(1).is_object() ? Web::Bindings::impl_from<Web::DOM::ParentNode>(&vm.argument(1).as_object()) : nullptr;
        if (!node)
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "Node");

        auto element = TRY(Web::WebIDL::throw_dom_exception_if_needed(vm, realm, [&]() {
            return node->query_selector(selector);
        }));
        if (!element)
            return JS::js_null();
        return Web::Bindings::wrap(Web::Bindings::host_defined_wrapper_world(realm), realm, element);
    }

    auto element = TRY(Web::WebIDL::throw_dom_exception_if_needed(vm, realm, [&]() {
        return window.associated_document().query_selector(selector);
    }));
    if (!element)
        return JS::js_null();
    return Web::Bindings::wrap(Web::Bindings::host_defined_wrapper_world(realm), realm, element);
}

JS_DEFINE_NATIVE_FUNCTION(ConsoleGlobalEnvironmentExtensions::$$_function)
{
    auto* extensions = TRY(get_console(vm));
    auto& realm = extensions->binding_object().shape().realm();
    auto& window = *extensions->m_window_object;

    auto selector = TRY(vm.argument(0).to_utf16_string(vm));

    Web::DOM::ParentNode* element = &window.associated_document();

    if (vm.argument_count() > 1) {
        auto node = vm.argument(1).is_object() ? Web::Bindings::impl_from<Web::DOM::ParentNode>(&vm.argument(1).as_object()) : nullptr;
        if (!node)
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "Node");
        element = node;
    }

    auto node_list = TRY(Web::WebIDL::throw_dom_exception_if_needed(vm, realm, [&]() {
        return element->query_selector_all(selector);
    }));

    auto array = TRY(JS::Array::create(realm, node_list->length()));
    for (auto i = 0u; i < node_list->length(); ++i) {
        auto* node = node_list->item(i);
        VERIFY(node);
        auto wrapped_node = Web::Bindings::wrap(Web::Bindings::host_defined_wrapper_world(realm), realm, GC::Ref { const_cast<Web::DOM::Node&>(*node) });
        TRY(array->create_data_property_or_throw(i, wrapped_node));
    }

    return array;
}

JS_DEFINE_NATIVE_FUNCTION(ConsoleGlobalEnvironmentExtensions::$x_function)
{
    auto* extensions = TRY(get_console(vm));
    auto& realm = extensions->binding_object().shape().realm();
    auto& window = *extensions->m_window_object;

    auto expression = TRY(vm.argument(0).to_utf16_string(vm));
    Web::DOM::Node* context_node = &window.associated_document();
    if (vm.argument_count() > 1 && vm.argument(1).is_object()) {
        if (auto* node = Web::Bindings::impl_from<Web::DOM::Node>(&vm.argument(1).as_object()))
            context_node = node;
    }

    auto result = TRY(Web::WebIDL::throw_dom_exception_if_needed(vm, realm, [&]() {
        return window.associated_document().evaluate(expression, *context_node, nullptr, Web::XPath::XPathResult::ORDERED_NODE_SNAPSHOT_TYPE, nullptr);
    }));

    auto length = TRY(Web::WebIDL::throw_dom_exception_if_needed(vm, realm, [&]() {
        return result->snapshot_length();
    }));

    auto array = TRY(JS::Array::create(realm, length));
    for (u32 i = 0; i < length; ++i) {
        auto node = TRY(Web::WebIDL::throw_dom_exception_if_needed(vm, realm, [&]() {
            return result->snapshot_item(i);
        }));
        if (!node)
            continue;
        auto wrapped = Web::Bindings::wrap(Web::Bindings::host_defined_wrapper_world(realm), realm, GC::Ref { *node });
        TRY(array->create_data_property_or_throw(i, wrapped));
    }
    return array;
}

JS_DEFINE_NATIVE_FUNCTION(ConsoleGlobalEnvironmentExtensions::keys_function)
{
    auto* extensions = TRY(get_console(vm));
    auto& realm = extensions->binding_object().shape().realm();
    auto object = TRY(vm.argument(0).to_object(vm));
    auto key_list = TRY(object->enumerable_own_property_names(JS::Object::PropertyKind::Key));
    return JS::Array::create_from(realm, key_list);
}

JS_DEFINE_NATIVE_FUNCTION(ConsoleGlobalEnvironmentExtensions::values_function)
{
    auto* extensions = TRY(get_console(vm));
    auto& realm = extensions->binding_object().shape().realm();
    auto object = TRY(vm.argument(0).to_object(vm));
    auto value_list = TRY(object->enumerable_own_property_names(JS::Object::PropertyKind::Value));
    return JS::Array::create_from(realm, value_list);
}

JS_DEFINE_NATIVE_FUNCTION(ConsoleGlobalEnvironmentExtensions::dir_function)
{
    auto* extensions = TRY(get_console(vm));
    auto& realm = extensions->binding_object().shape().realm();
    auto console = realm.intrinsics().console_object();
    return JS::call(vm, TRY(console->get("dir"_utf16_fly_string)), console, vm.argument(0));
}

JS_DEFINE_NATIVE_FUNCTION(ConsoleGlobalEnvironmentExtensions::table_function)
{
    auto* extensions = TRY(get_console(vm));
    auto& realm = extensions->binding_object().shape().realm();
    auto console = realm.intrinsics().console_object();
    return JS::call(vm, TRY(console->get("table"_utf16_fly_string)), console, vm.argument(0), vm.argument(1));
}

JS_DEFINE_NATIVE_FUNCTION(ConsoleGlobalEnvironmentExtensions::clear_function)
{
    auto* extensions = TRY(get_console(vm));
    auto& realm = extensions->binding_object().shape().realm();
    auto console = realm.intrinsics().console_object();
    return JS::call(vm, TRY(console->get("clear"_utf16_fly_string)), console);
}

JS_DEFINE_NATIVE_FUNCTION(ConsoleGlobalEnvironmentExtensions::copy_function)
{
    auto* extensions = TRY(get_console(vm));
    auto& window = *extensions->m_window_object;
    auto value = vm.argument(0);

    String text;
    if (value.is_string()) {
        text = value.as_string().utf16_string().to_utf8();
    } else if (value.is_object()) {
        auto json_or_err = JS::JSONObject::stringify_impl(vm, value, JS::js_undefined(), JS::Value(2));
        if (!json_or_err.is_error() && json_or_err.value().has_value())
            text = json_or_err.value()->to_utf8();
        else
            text = TRY(value.to_utf16_string(vm)).to_utf8();
    } else {
        text = TRY(value.to_utf16_string(vm)).to_utf8();
    }

    Web::Clipboard::SystemClipboardItem item;
    item.system_clipboard_representations.append({ "text/plain"_string, text.to_byte_string() });
    window.page().client().page_did_insert_clipboard_item(item, "unspecified"sv, window.associated_document().id());
    return JS::js_undefined();
}

JS_DEFINE_NATIVE_FUNCTION(ConsoleGlobalEnvironmentExtensions::inspect_function)
{
    auto* extensions = TRY(get_console(vm));
    auto& window = *extensions->m_window_object;
    auto arg = vm.argument(0);
    if (arg.is_object()) {
        if (auto* node = Web::Bindings::impl_from<Web::DOM::Node>(&arg.as_object()); node && &node->document() == &window.associated_document())
            window.associated_document().set_inspector_selected_node(node);
    }
    return arg;
}

}
