#include "board.hpp"
#include "canvas/canvas_patch.hpp"

namespace horizon {

	static void polynode_to_fragment(Plane *plane, const ClipperLib::PolyNode *node) {
		assert(node->IsHole()==false);
		plane->fragments.emplace_back();
		auto &fragment = plane->fragments.back();
		fragment.paths.emplace_back();
		auto &outer = fragment.paths.back();
		outer = node->Contour; //first path is countour

		for(auto child: node->Childs) {
			assert(child->IsHole()==true);

			fragment.paths.emplace_back();
			auto &hole = fragment.paths.back();
			hole = child->Contour;

			for(auto child2: child->Childs) { //add fragments in holes
				polynode_to_fragment(plane, child2);
			}
		}
	}


	void Board::update_plane(Plane *plane, CanvasPatch *ca_ext) {
		CanvasPatch ca_my;
		CanvasPatch *ca = ca_ext;
		if(!ca_ext) {
			ca_my.update(*this);
			ca = &ca_my;
		}
		plane->fragments.clear();

		ClipperLib::Clipper cl_plane;
		ClipperLib::Path poly_path; //path from polygon contour
		auto poly = plane->polygon->remove_arcs();
		poly_path.reserve(poly.vertices.size());
		for(const auto &pt: poly.vertices) {
			poly_path.emplace_back(ClipperLib::IntPoint(pt.position.x, pt.position.y));
		}
		ClipperLib::JoinType jt = plane->settings.style==PlaneSettings::Style::ROUND?ClipperLib::jtRound:ClipperLib::jtSquare;

		{
			ClipperLib::ClipperOffset ofs_poly; //shrink polygon contour by min_width/2
			ofs_poly.ArcTolerance = 2e3;
			ofs_poly.AddPath(poly_path, jt, ClipperLib::etClosedPolygon);
			ClipperLib::Paths poly_shrink;
			ofs_poly.Execute(poly_shrink, -((double)plane->settings.min_width)/2);
			cl_plane.AddPaths(poly_shrink, ClipperLib::ptSubject, true);
		}

		double twiddle = .005_mm;

		for(const auto &patch: ca->patches) { // add cutouts
			if((patch.first.layer == poly.layer && patch.first.net != plane->net->uuid && patch.first.type != PatchType::OTHER) ||
			   (patch.first.layer == 10000 && patch.first.type == PatchType::HOLE_NPTH)) {
				ClipperLib::ClipperOffset ofs; //expand patch for cutout
				ofs.ArcTolerance = 2e3;
				ofs.AddPaths(patch.second, jt, ClipperLib::etClosedPolygon);
				ClipperLib::Paths patch_exp;

				int64_t clearance = 0;
				if(patch.first.type != PatchType::HOLE_NPTH) { //copper
					auto patch_net= patch.first.net?&block->nets.at(patch.first.net):nullptr;
					auto rule_clearance = rules.get_clearance_copper(plane->net, patch_net, poly.layer);
					if(rule_clearance) {
						clearance = rule_clearance->get_clearance(patch.first.type, PatchType::PLANE);
					}
				}
				else { //npth
					clearance = rules.get_clearance_npth_copper(plane->net, poly.layer);
				}
				double expand = clearance+plane->settings.min_width/2+twiddle;

				ofs.Execute(patch_exp, expand);
				cl_plane.AddPaths(patch_exp, ClipperLib::ptClip, true);
			}
		}
		ClipperLib::Paths out; //the plane before expanding by min_width
		cl_plane.Execute(ClipperLib::ctDifference, out, ClipperLib::pftNonZero); //do cutouts

		ClipperLib::PolyTree tree;
		ClipperLib::ClipperOffset ofs;
		ofs.ArcTolerance = 2e3;
		ofs.AddPaths(out, jt, ClipperLib::etClosedPolygon);
		ofs.Execute(tree, plane->settings.min_width/2);
		for(const auto node: tree.Childs) {
			polynode_to_fragment(plane, node);
		}

		//orphan all
		for(auto &it: plane->fragments) {
			it.orphan = true;
		}

		//find orphans
		for(auto &frag: plane->fragments) {
			for(const auto &it: junctions) {
				if(it.second.net == plane->net && (it.second.layer == plane->polygon->layer || it.second.has_via) && frag.contains(it.second.position)) {
					frag.orphan = false;
					break;
				}
			}
			if(frag.orphan) { //still orphan, check pads
				for(auto &it: packages) {
					auto pkg = &it.second;
					for(auto &it_pad: it.second.package.pads) {
						auto pad = &it_pad.second;
						if(pad->net == plane->net) {
							Track::Connection conn(pkg, pad);
							auto pos = conn.get_position();
							if(frag.contains(pos)) {
								frag.orphan = false;
								break;
							}
						}
					}
					if(!frag.orphan) { //don't need to check other packages
						break;
					}
				}
			}
		}

		if(!plane->settings.keep_orphans) { //delete orphans
			plane->fragments.erase(std::remove_if(plane->fragments.begin(), plane->fragments.end(), [](const auto &x){return x.orphan;}), plane->fragments.end());
		}
	}

	void Board::update_planes() {
		std::vector<Plane*> planes_sorted;
		planes_sorted.reserve(planes.size());
		for(auto &it: planes) {
			it.second.fragments.clear();
			planes_sorted.push_back(&it.second);
		}
		std::sort(planes_sorted.begin(), planes_sorted.end(), [](const auto a, const auto b){return a->priority < b->priority;});

		CanvasPatch ca;
		ca.update(*this);
		for(auto plane: planes_sorted) {
			update_plane(plane, &ca);
			ca.update(*this); //update so that new plane sees other planes
		}
	}
}
