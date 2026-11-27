/*
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Forward.h>

namespace WebContent {

// The state behind the console's $0, $_, $ and $$ helpers. The helpers themselves are properties of a host object that
// carries this cell as its host data, and console input sees them through a with-scope over that object.
class ConsoleGlobalEnvironmentExtensions final : public JS::Cell {
    GC_CELL(ConsoleGlobalEnvironmentExtensions, JS::Cell);
    GC_DECLARE_ALLOCATOR(ConsoleGlobalEnvironmentExtensions);

public:
    using JSValueConversionIsForbidden = void;

    static GC::Ref<ConsoleGlobalEnvironmentExtensions> create(JS::Realm&, Web::HTML::Window&);
    virtual ~ConsoleGlobalEnvironmentExtensions() override = default;

    JS::Object& binding_object() const { return *m_binding_object; }

    void set_most_recent_result(JS::Value result) { m_most_recent_result = move(result); }

private:
    explicit ConsoleGlobalEnvironmentExtensions(Web::HTML::Window&);

    virtual void visit_edges(Visitor&) override;

    static JS::ThrowCompletionOr<JS::Value> get_inspected_node_at(JS::VM&, size_t);

    // $0..$4, the last 5 DOM nodes selected in the inspector
    JS_DECLARE_NATIVE_FUNCTION($0_getter);
    JS_DECLARE_NATIVE_FUNCTION($1_getter);
    JS_DECLARE_NATIVE_FUNCTION($2_getter);
    JS_DECLARE_NATIVE_FUNCTION($3_getter);
    JS_DECLARE_NATIVE_FUNCTION($4_getter);
    // $_, the value of the most recent expression entered into the console
    JS_DECLARE_NATIVE_FUNCTION($__getter);
    // $(selector, element), equivalent to `(element || document).querySelector(selector)`
    JS_DECLARE_NATIVE_FUNCTION($_function);
    // $$(selector, element), equivalent to `(element || document).querySelectorAll(selector)`
    JS_DECLARE_NATIVE_FUNCTION($$_function);
    // $x(xpath, contextNode), evaluates an XPath expression returning matching nodes
    JS_DECLARE_NATIVE_FUNCTION($x_function);
    // keys(obj), values(obj), dir(obj), table(data), clear(), copy(val), inspect(node)
    JS_DECLARE_NATIVE_FUNCTION(keys_function);
    JS_DECLARE_NATIVE_FUNCTION(values_function);
    JS_DECLARE_NATIVE_FUNCTION(dir_function);
    JS_DECLARE_NATIVE_FUNCTION(table_function);
    JS_DECLARE_NATIVE_FUNCTION(clear_function);
    JS_DECLARE_NATIVE_FUNCTION(copy_function);
    JS_DECLARE_NATIVE_FUNCTION(inspect_function);

    GC::Ref<Web::HTML::Window> m_window_object;
    GC::Ptr<JS::Object> m_binding_object;
    JS::Value m_most_recent_result { JS::js_undefined() };
};

}
