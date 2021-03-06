#include "imp_board.hpp"
#include "3d_view.hpp"
#include "canvas/canvas_gl.hpp"
#include "canvas/canvas_pads.hpp"
#include "fab_output_window.hpp"
#include "step_export_window.hpp"
#include "pool/part.hpp"
#include "rules/rules_window.hpp"
#include "widgets/board_display_options.hpp"
#include "widgets/layer_box.hpp"
#include "tuning_window.hpp"
#include "hud_util.hpp"
#include "util/util.hpp"
#include "util/str_util.hpp"
#include "canvas/annotation.hpp"

namespace horizon {
ImpBoard::ImpBoard(const std::string &board_filename, const std::string &block_filename, const std::string &via_dir,
                   const PoolParams &pool_params)
    : ImpLayer(pool_params), core_board(board_filename, block_filename, via_dir, *pool)
{
    core = &core_board;
    core_board.signal_tool_changed().connect(sigc::mem_fun(this, &ImpBase::handle_tool_change));
}

void ImpBoard::canvas_update()
{
    canvas->update(*core_board.get_canvas_data());
    warnings_box->update(core_board.get_board()->warnings);
    update_highlights();
    tuning_window->update();
    update_text_owner_annotation();
}

void ImpBoard::update_highlights()
{
    canvas->set_flags_all(0, Triangle::FLAG_HIGHLIGHT);
    canvas->set_highlight_enabled(highlights.size());
    for (const auto &it : highlights) {
        if (it.type == ObjectType::NET) {
            for (const auto &it_track : core_board.get_board()->tracks) {
                if (it_track.second.net.uuid == it.uuid) {
                    ObjectRef ref(ObjectType::TRACK, it_track.first);
                    canvas->set_flags(ref, Triangle::FLAG_HIGHLIGHT, 0);
                }
            }
            for (const auto &it_via : core_board.get_board()->vias) {
                if (it_via.second.junction->net.uuid == it.uuid) {
                    ObjectRef ref(ObjectType::VIA, it_via.first);
                    canvas->set_flags(ref, Triangle::FLAG_HIGHLIGHT, 0);
                }
            }
            for (const auto &it_pkg : core_board.get_board()->packages) {
                for (const auto &it_pad : it_pkg.second.package.pads) {
                    if (it_pad.second.net.uuid == it.uuid) {
                        ObjectRef ref(ObjectType::PAD, it_pad.first, it_pkg.first);
                        canvas->set_flags(ref, Triangle::FLAG_HIGHLIGHT, 0);
                    }
                }
            }
        }
        if (it.type == ObjectType::COMPONENT) {
            for (const auto &it_pkg : core_board.get_board()->packages) {
                if (it_pkg.second.component->uuid == it.uuid) {
                    ObjectRef ref(ObjectType::BOARD_PACKAGE, it_pkg.first);
                    canvas->set_flags(ref, Triangle::FLAG_HIGHLIGHT, 0);
                }
            }
        }
        else {
            canvas->set_flags(it, Triangle::FLAG_HIGHLIGHT, 0);
        }
    }
}

bool ImpBoard::handle_broadcast(const json &j)
{
    if (!ImpBase::handle_broadcast(j)) {
        std::string op = j.at("op");
        if (op == "highlight" && cross_probing_enabled && !core_board.tool_is_active()) {
            highlights.clear();
            const json &o = j["objects"];
            for (auto it = o.cbegin(); it != o.cend(); ++it) {
                auto type = static_cast<ObjectType>(it.value().at("type").get<int>());
                UUID uu(it.value().at("uuid").get<std::string>());
                highlights.emplace(type, uu);
            }
            update_highlights();
        }
        else if (op == "place" && !core_board.tool_is_active()) {
            main_window->present();
            std::set<SelectableRef> components;
            const json &o = j["components"];
            for (auto it = o.cbegin(); it != o.cend(); ++it) {
                auto type = static_cast<ObjectType>(it.value().at("type").get<int>());
                if (type == ObjectType::COMPONENT) {
                    UUID uu(it.value().at("uuid").get<std::string>());
                    components.emplace(uu, type);
                }
            }
            highlights.clear();
            update_highlights();
            ToolArgs args;
            args.coords = canvas->get_cursor_pos();
            args.selection = components;
            args.work_layer = canvas->property_work_layer();
            ToolResponse r = core.r->tool_begin(ToolID::MAP_PACKAGE, args, imp_interface.get());
            tool_process(r);
        }
        else if (op == "reload-netlist" && !core_board.tool_is_active()) {
            main_window->present();
            trigger_action(ActionID::RELOAD_NETLIST);
        }
    }
    return true;
}

void ImpBoard::handle_selection_cross_probe()
{
    json j;
    j["op"] = "board-select";
    j["selection"] = nullptr;
    std::set<UUID> pkgs;
    for (const auto &it : canvas->get_selection()) {
        json k;
        ObjectType type = ObjectType::INVALID;
        UUID uu;
        auto board = core_board.get_board();
        switch (it.type) {
        case ObjectType::TRACK: {
            auto &track = board->tracks.at(it.uuid);
            if (track.net) {
                type = ObjectType::NET;
                uu = track.net->uuid;
            }
        } break;
        case ObjectType::VIA: {
            auto &via = board->vias.at(it.uuid);
            if (via.junction->net) {
                type = ObjectType::NET;
                uu = via.junction->net->uuid;
            }
        } break;
        case ObjectType::JUNCTION: {
            auto &ju = board->junctions.at(it.uuid);
            if (ju.net) {
                type = ObjectType::NET;
                uu = ju.net->uuid;
            }
        } break;
        case ObjectType::BOARD_PACKAGE: {
            auto &pkg = board->packages.at(it.uuid);
            type = ObjectType::COMPONENT;
            uu = pkg.component->uuid;
            pkgs.insert(pkg.uuid);
        } break;
        default:;
        }

        if (type != ObjectType::INVALID) {
            k["type"] = static_cast<int>(type);
            k["uuid"] = (std::string)uu;
            j["selection"].push_back(k);
        }
    }
    view_3d_window->set_highlights(pkgs);
    send_json(j);
}

void transform_path(ClipperLib::Path &path, const Placement &tr)
{
    for (auto &it : path) {
        Coordi p(it.X, it.Y);
        auto q = tr.transform(p);
        it.X = q.x;
        it.Y = q.y;
    }
}

void ImpBoard::update_action_sensitivity()
{
    auto sel = canvas->get_selection();
    bool have_tracks = std::any_of(sel.begin(), sel.end(), [](const auto &x) { return x.type == ObjectType::TRACK; });
    set_action_sensitive(make_action(ActionID::TUNING_ADD_TRACKS), have_tracks);
    set_action_sensitive(make_action(ActionID::TUNING_ADD_TRACKS_ALL), have_tracks);

    set_action_sensitive(make_action(ActionID::HIGHLIGHT_NET), std::any_of(sel.begin(), sel.end(), [](const auto &x) {
                             switch (x.type) {
                             case ObjectType::TRACK:
                             case ObjectType::JUNCTION:
                             case ObjectType::VIA:
                                 return true;

                             default:
                                 return false;
                             }
                         }));

    ImpBase::update_action_sensitivity();
}

void ImpBoard::construct()
{
    ImpLayer::construct_layer_box(false);

    main_window->set_title("Board - Interactive Manipulator");
    state_store = std::make_unique<WindowStateStore>(main_window, "imp-board");

    auto view_3d_button = Gtk::manage(new Gtk::Button("3D"));
    main_window->header->pack_start(*view_3d_button);
    view_3d_button->show();
    view_3d_button->signal_clicked().connect([this] {
        view_3d_window->update();
        view_3d_window->present();
    });

    auto hamburger_menu = add_hamburger_menu();

    hamburger_menu->append("Fabrication output", "win.fab_out");
    main_window->add_action("fab_out", [this] { fab_output_window->present(); });

    hamburger_menu->append("Stackup...", "win.edit_stackup");
    add_tool_action(ToolID::EDIT_STACKUP, "edit_stackup");

    hamburger_menu->append("Update all planes", "win.update_all_planes");
    add_tool_action(ToolID::UPDATE_ALL_PLANES, "update_all_planes");

    hamburger_menu->append("Clear all planes", "win.clear_all_planes");
    add_tool_action(ToolID::CLEAR_ALL_PLANES, "clear_all_planes");

    hamburger_menu->append("Import DXF", "win.import_dxf");
    add_tool_action(ToolID::IMPORT_DXF, "import_dxf");

    hamburger_menu->append("Export STEP", "win.export_step");
    main_window->add_action("export_step", [this] { step_export_window->present(); });

    hamburger_menu->append("Length tuning", "win.tuning");
    main_window->add_action("tuning", [this] { trigger_action(ActionID::TUNING); });


    if (sockets_connected) {
        hamburger_menu->append("Cross probing", "win.cross_probing");
        auto cp_action = main_window->add_action_bool("cross_probing", true);
        cross_probing_enabled = true;
        cp_action->signal_change_state().connect([this, cp_action](const Glib::VariantBase &v) {
            cross_probing_enabled = Glib::VariantBase::cast_dynamic<Glib::Variant<bool>>(v).get();
            g_simple_action_set_state(cp_action->gobj(), g_variant_new_boolean(cross_probing_enabled));
            if (!cross_probing_enabled && !core_board.tool_is_active()) {
                highlights.clear();
                update_highlights();
            }
        });
    }

    add_tool_button(ToolID::MAP_PACKAGE, "Place package", false);

    connect_action(ActionID::RELOAD_NETLIST, [this](const ActionConnection &c) {
        core_board.reload_netlist();
        core_board.set_needs_save();
        canvas_update();
    });

    {
        auto button = create_action_button(make_action(ActionID::RELOAD_NETLIST));
        button->show();
        main_window->header->pack_end(*button);
    }

    if (sockets_connected) {
        canvas->signal_selection_changed().connect(sigc::mem_fun(this, &ImpBoard::handle_selection_cross_probe));
    }

    fab_output_window = FabOutputWindow::create(main_window, core.b->get_board(), core.b->get_fab_output_settings());
    view_3d_window = View3DWindow::create(core_board.get_board(), pool.get());
    step_export_window = StepExportWindow::create(main_window, core.b->get_board(), pool.get());
    tuning_window = new TuningWindow(core.b->get_board());
    tuning_window->set_transient_for(*main_window);

    core.r->signal_tool_changed().connect([this](ToolID t) { fab_output_window->set_can_generate(t == ToolID::NONE); });

    rules_window->signal_goto().connect([this](Coordi location, UUID sheet) { canvas->center_and_zoom(location); });

    connect_action(ActionID::VIEW_3D, [this](const auto &a) {
        view_3d_window->update();
        view_3d_window->present();
    });

    connect_action(ActionID::TUNING, [this](const auto &a) { tuning_window->present(); });
    connect_action(ActionID::TUNING_ADD_TRACKS, sigc::mem_fun(this, &ImpBoard::handle_measure_tracks));
    connect_action(ActionID::TUNING_ADD_TRACKS_ALL, sigc::mem_fun(this, &ImpBoard::handle_measure_tracks));

    connect_action(ActionID::HIGHLIGHT_NET, [this](const auto &a) {
        highlights.clear();
        for (const auto &it : canvas->get_selection()) {
            auto board = core_board.get_board();
            switch (it.type) {
            case ObjectType::TRACK: {
                auto &track = board->tracks.at(it.uuid);
                if (track.net) {
                    highlights.emplace(ObjectType::NET, track.net->uuid);
                }
            } break;
            case ObjectType::VIA: {
                auto &via = board->vias.at(it.uuid);
                if (via.junction->net) {
                    highlights.emplace(ObjectType::NET, via.junction->net->uuid);
                }
            } break;
            case ObjectType::JUNCTION: {
                auto &ju = board->junctions.at(it.uuid);
                if (ju.net) {
                    highlights.emplace(ObjectType::NET, ju.net->uuid);
                }
            } break;
            default:;
            }
        }
        this->update_highlights();
    });

    auto *display_control_notebook = Gtk::manage(new Gtk::Notebook);
    display_control_notebook->append_page(*layer_box, "Layers");
    layer_box->show();
    layer_box->get_style_context()->add_class("background");

    auto board_display_options = Gtk::manage(new BoardDisplayOptionsBox(core.b->get_layer_provider()));
    {
        auto fbox = Gtk::manage(new Gtk::Box());
        fbox->pack_start(*board_display_options, true, true, 0);
        fbox->get_style_context()->add_class("background");
        fbox->show();
        display_control_notebook->append_page(*fbox, "Options");
        board_display_options->show();
    }

    board_display_options->signal_set_layer_display().connect([this](int index, const LayerDisplay &lda) {
        LayerDisplay ld = canvas->get_layer_display(index);
        ld.types_force_outline = lda.types_force_outline;
        ld.types_visible = lda.types_visible;
        canvas->set_layer_display(index, ld);
        canvas->queue_draw();
    });
    canvas->set_layer_display(10000, LayerDisplay(true, LayerDisplay::Mode::OUTLINE, Color(1, 1, 1)));
    core.r->signal_rebuilt().connect([board_display_options] { board_display_options->update(); });

    canvas->signal_motion_notify_event().connect([this](GdkEventMotion *ev) {
        if (target_drag_begin.type != ObjectType::INVALID) {
            handle_drag();
        }
        return false;
    });

    canvas->signal_button_release_event().connect([this](GdkEventButton *ev) {
        target_drag_begin = Target();
        return false;
    });

    text_owner_annotation = canvas->create_annotation();
    text_owner_annotation->set_visible(true);
    text_owner_annotation->set_display(LayerDisplay(true, LayerDisplay::Mode::OUTLINE, Color(1, 1, 0)));

    core_board.signal_rebuilt().connect(sigc::mem_fun(this, &ImpBoard::update_text_owners));
    canvas->signal_hover_selection_changed().connect(sigc::mem_fun(this, &ImpBoard::update_text_owner_annotation));
    canvas->signal_selection_changed().connect(sigc::mem_fun(this, &ImpBoard::update_text_owner_annotation));
    update_text_owners();

    main_window->left_panel->pack_start(*display_control_notebook, false, false);
    display_control_notebook->show();
}

void ImpBoard::update_text_owners()
{
    auto brd = core_board.get_board();
    for (const auto &it_pkg : brd->packages) {
        for (const auto &itt : it_pkg.second.texts) {
            text_owners[itt->uuid] = it_pkg.first;
        }
    }
    update_text_owner_annotation();
}

void ImpBoard::update_text_owner_annotation()
{
    text_owner_annotation->clear();
    auto sel = canvas->get_selection();
    const auto brd = core_board.get_board();
    for (const auto &it : sel) {
        if (it.type == ObjectType::TEXT) {
            if (brd->texts.count(it.uuid)) {
                const auto &text = brd->texts.at(it.uuid);
                if (text_owners.count(text.uuid))
                    text_owner_annotation->draw_line(text.placement.shift,
                                                     brd->packages.at(text_owners.at(text.uuid)).placement.shift,
                                                     ColorP::FROM_LAYER, 0);
            }
        }
        else if (it.type == ObjectType::BOARD_PACKAGE) {
            if (brd->packages.count(it.uuid)) {
                const auto &pkg = brd->packages.at(it.uuid);
                for (const auto &text : pkg.texts) {
                    if (canvas->layer_is_visible(text->layer))
                        text_owner_annotation->draw_line(text->placement.shift, pkg.placement.shift, ColorP::FROM_LAYER,
                                                         0);
                }
            }
        }
    }
}

std::string ImpBoard::get_hud_text(std::set<SelectableRef> &sel)
{
    std::string s;
    if (sel_count_type(sel, ObjectType::TRACK)) {
        auto n = sel_count_type(sel, ObjectType::TRACK);
        s += "\n\n<b>" + std::to_string(n) + " " + object_descriptions.at(ObjectType::TRACK).get_name_for_n(n)
             + "</b>\n";
        std::set<int> layers;
        std::set<const Net *> nets;
        int64_t length = 0;
        const Track *the_track = nullptr;
        for (const auto &it : sel) {
            if (it.type == ObjectType::TRACK) {
                const auto &tr = core_board.get_board()->tracks.at(it.uuid);
                the_track = &tr;
                layers.insert(tr.layer);
                length += sqrt((tr.from.get_position() - tr.to.get_position()).mag_sq());
                if (tr.net)
                    nets.insert(tr.net);
            }
        }
        s += "Layers: ";
        for (auto layer : layers) {
            s += core.r->get_layer_provider()->get_layers().at(layer).name + " ";
        }
        s += "\nTotal length: " + dim_to_string(length, false);
        if (sel_count_type(sel, ObjectType::TRACK) == 1) {
            s += "\n" + get_hud_text_for_net(the_track->net);
        }
        else {
            s += "\n" + std::to_string(nets.size()) + " "
                 + object_descriptions.at(ObjectType::NET).get_name_for_n(nets.size());
        }
        sel_erase_type(sel, ObjectType::TRACK);
    }
    trim(s);
    if (sel_count_type(sel, ObjectType::BOARD_PACKAGE) == 1) {
        const auto &pkg = core_board.get_board()->packages.at(sel_find_one(sel, ObjectType::BOARD_PACKAGE));
        s += "\n\n<b>Package " + pkg.component->refdes + "</b>\n";
        s += get_hud_text_for_part(pkg.component->part);
        sel_erase_type(sel, ObjectType::BOARD_PACKAGE);
    }
    else if (sel_count_type(sel, ObjectType::BOARD_PACKAGE) > 1) {
        auto n = sel_count_type(sel, ObjectType::BOARD_PACKAGE);
        s += "\n\n<b>" + std::to_string(n) + " " + object_descriptions.at(ObjectType::BOARD_PACKAGE).get_name_for_n(n)
             + "</b>\n";
        size_t n_pads = 0;
        for (const auto &it : sel) {
            if (it.type == ObjectType::BOARD_PACKAGE) {
                const auto &pkg = core_board.get_board()->packages.at(it.uuid);
                n_pads += pkg.package.pads.size();
            }
        }
        s += "Total pads: " + std::to_string(n_pads);
        sel_erase_type(sel, ObjectType::BOARD_PACKAGE);
    }
    trim(s);
    return s;
}

void ImpBoard::handle_measure_tracks(const ActionConnection &a)
{
    auto sel = canvas->get_selection();
    std::set<UUID> tracks;
    for (const auto &it : sel) {
        if (it.type == ObjectType::TRACK) {
            tracks.insert(it.uuid);
        }
    }
    tuning_window->add_tracks(tracks, a.action_id == ActionID::TUNING_ADD_TRACKS_ALL);
    tuning_window->present();
}

void ImpBoard::handle_maybe_drag()
{
    if (!preferences.board.drag_start_track)
        return;
    auto target = canvas->get_current_target();
    auto brd = core_board.get_board();
    if (target.type == ObjectType::PAD) {
        auto pkg = brd->packages.at(target.path.at(0));
        auto pad = pkg.package.pads.at(target.path.at(1));
        if (pad.net == nullptr)
            return;
    }
    else if (target.type == ObjectType::JUNCTION) {
        auto ju = brd->junctions.at(target.path.at(0));
        if (ju.net == nullptr)
            return;
    }
    else {
        ImpBase::handle_maybe_drag();
        return;
    }
    canvas->inhibit_drag_selection();
    target_drag_begin = target;
    cursor_pos_drag_begin = canvas->get_cursor_pos_win();
}

void ImpBoard::handle_drag()
{
    auto pos = canvas->get_cursor_pos_win();
    auto delta = pos - cursor_pos_drag_begin;
    if (delta.mag_sq() > (50 * 50)) {
        {
            highlights.clear();
            update_highlights();
            ToolArgs args;
            args.coords = target_drag_begin.p;
            ToolResponse r = core.r->tool_begin(ToolID::ROUTE_TRACK_INTERACTIVE, args, imp_interface.get());
            tool_process(r);
        }
        {
            ToolArgs args;
            args.type = ToolEventType::CLICK;
            args.coords = target_drag_begin.p;
            args.button = 1;
            args.target = target_drag_begin;
            args.work_layer = canvas->property_work_layer();
            ToolResponse r = core.r->tool_update(args);
            tool_process(r);
        }
        target_drag_begin = Target();
    }
}
} // namespace horizon
