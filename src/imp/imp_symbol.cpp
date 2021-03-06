#include "imp_symbol.hpp"
#include "canvas/canvas_gl.hpp"
#include "header_button.hpp"
#include "pool/part.hpp"
#include "symbol_preview/symbol_preview_window.hpp"

namespace horizon {
ImpSymbol::ImpSymbol(const std::string &symbol_filename, const std::string &pool_path)
    : ImpBase(pool_path), core_symbol(symbol_filename, *pool)
{
    core = &core_symbol;
    core_symbol.signal_tool_changed().connect(sigc::mem_fun(this, &ImpBase::handle_tool_change));
}

void ImpSymbol::canvas_update()
{
    canvas->update(*core_symbol.get_canvas_data());
    symbol_preview_window->update(*core_symbol.get_canvas_data());
}

void ImpSymbol::update_monitor()
{
    set_monitor_items({{ObjectType::UNIT, core_symbol.get_symbol()->unit->uuid}});
}

void ImpSymbol::construct()
{

    main_window->set_title("Symbol - Interactive Manipulator");
    state_store = std::make_unique<WindowStateStore>(main_window, "imp-symbol");

    symbol_preview_window = new SymbolPreviewWindow(main_window);
    symbol_preview_window->set_text_placements(core.y->get_symbol(false)->text_placements);
    symbol_preview_window->signal_changed().connect([this] { core_symbol.set_needs_save(); });

    {
        auto button = Gtk::manage(new Gtk::Button("Preview..."));
        main_window->header->pack_start(*button);
        button->show();
        button->signal_clicked().connect([this] { symbol_preview_window->present(); });
    }

    auto header_button = Gtk::manage(new HeaderButton);
    header_button->set_label(core.y->get_symbol()->name);
    main_window->header->set_custom_title(*header_button);
    header_button->show();

    name_entry = header_button->add_entry("Name");
    name_entry->set_text(core.y->get_symbol()->name);
    name_entry->set_width_chars(std::min<int>(core.y->get_symbol()->name.size(), 20));
    name_entry->signal_changed().connect([this, header_button] {
        header_button->set_label(name_entry->get_text());
        core_symbol.set_needs_save();
    });

    {
        unit_label = Gtk::manage(new Gtk::Label(core.y->get_symbol()->unit->name));
        unit_label->set_xalign(0);
        unit_label->set_selectable(true);
        header_button->add_widget("Unit", unit_label);
    }

    core.r->signal_rebuilt().connect([this] { unit_label->set_text(core.y->get_symbol()->unit->name); });

    core.r->signal_save().connect([this, header_button] {
        auto sym = core.y->get_symbol(false);
        sym->name = name_entry->get_text();
        header_button->set_label(sym->name);
        sym->text_placements = symbol_preview_window->get_text_placements();
    });
    grid_spin_button->set_sensitive(false);
    update_monitor();

    connect_action(ActionID::EDIT_UNIT,
                   [this](const auto &a) { edit_pool_item(ObjectType::UNIT, core_symbol.get_symbol()->unit->uuid); });
    set_action_sensitive(make_action(ActionID::EDIT_UNIT), sockets_connected);

    auto hamburger_menu = add_hamburger_menu();
    hamburger_menu->append("Change unit", "win.change_unit");
    add_tool_action(ToolID::CHANGE_UNIT, "change_unit");

    if (sockets_connected) {
        hamburger_menu->append("Edit unit", "win.edit_unit");
        main_window->add_action("edit_unit", [this] { trigger_action(ActionID::EDIT_UNIT); });
    }
}

} // namespace horizon
