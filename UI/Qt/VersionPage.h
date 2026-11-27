/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <UI/Qt/WebUIPage.h>

namespace Ladybird {

class VersionPage final : public WebUIPage {
public:
    explicit VersionPage(u64 web_ui_id);
    virtual ~VersionPage() override;

    virtual void received_message(String const& name, JsonValue const& data) override;

    static void notify_accent_changed();

private:
    void load_version_info();
};

}
