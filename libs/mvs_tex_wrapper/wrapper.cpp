#include "mvs_tex_wrapper/wrapper.h"


#include <iostream>
#include <fstream>
#include <vector>

#include <util/timer.h>
#include <util/system.h>
#include <util/file_system.h>
#include <mve/mesh_io_ply.h>

#include "tex/debug.h"
#include "tex/progress_counter.h"
#include "tex/settings.h"
#include "tex/texturing.h"
#include "tex/texture_patch.h"
#include "tex/timer.h"
#include "tex/util.h"


namespace MvsTexturing {

void textureMesh(const TextureSettings& texture_settings,
                 const std::string& in_scene,
                 const std::string& in_mesh,
                 const std::string& out_prefix,
                 const std::vector<std::vector<bool>>& sub_vert_masks,
                 const std::vector<std::string>& sub_names,
                 std::shared_ptr<EuclideanViewMask> ev_mask,
                 uint atlas_size,
                 float* hidden_face_proportion) {
    bool write_timings = false;
    bool write_intermediate_results = false;
    bool write_view_selection_model = false;

    std::cout << "Texturing ...\n Eigen version:" << std::endl;
    std::cout << EIGEN_WORLD_VERSION << "." << EIGEN_MAJOR_VERSION << "." << EIGEN_MINOR_VERSION<< std::endl;

    std::string data_cost_file = "";
    std::string labeling_file = "";

    Timer timer;
    util::WallTimer wtimer;

    //
    // Prep Filesystem + load data
    //

    if (atlas_size == 0)
        atlas_size = 4096;
    std::string const out_dir = util::fs::dirname(out_prefix);

    if (!util::fs::dir_exists(out_dir.c_str())) {
        std::cerr << "Destination directory does not exist!" << std::endl;
        std::exit(EXIT_FAILURE);
    }

    std::string const tmp_dir = util::fs::join_path(out_dir, "tmp");
    if (!util::fs::dir_exists(tmp_dir.c_str())) {
        util::fs::mkdir(tmp_dir.c_str());
    }

    std::cout << "Load and prepare mesh: " << std::endl;
    mve::TriangleMesh::Ptr mesh;
    try {
        mesh = mve::geom::load_ply_mesh(in_mesh);
    } catch (std::exception& e) {
        std::cerr << "\tCould not load mesh: " << e.what() << std::endl;
        std::exit(EXIT_FAILURE);
    }
    mve::MeshInfo mesh_info(mesh);
    tex::prepare_mesh(&mesh_info, mesh);

    std::cout << "Generating texture views: " << std::endl;
    tex::TextureViews texture_views;
    tex::generate_texture_views(in_scene, &texture_views, tmp_dir);

    timer.measure("Loading");

    std::size_t const num_faces = mesh->get_faces().size() / 3;

    std::cout << "Building adjacency graph: " << std::endl;
    tex::Graph graph(num_faces);
    tex::build_adjacency_graph(mesh, mesh_info, &graph);

    //
    // Build Processing Settings
    //
    tex::Settings settings;
    if (texture_settings.do_use_gmi_term)
        settings.data_term = tex::DATA_TERM_GMI;
    else
        settings.data_term = tex::DATA_TERM_AREA;

    if (texture_settings.do_gauss_clamping)
        settings.outlier_removal = tex::OUTLIER_REMOVAL_GAUSS_CLAMPING;
    else if (texture_settings.do_gauss_damping)
        settings.outlier_removal = tex::OUTLIER_REMOVAL_GAUSS_DAMPING;
    else
        settings.outlier_removal = tex::OUTLIER_REMOVAL_NONE;

    if (texture_settings.do_gamma_tone_mapping)
        settings.tone_mapping = tex::TONE_MAPPING_GAMMA;
    else
        settings.tone_mapping = tex::TONE_MAPPING_NONE;

    settings.geometric_visibility_test = texture_settings.do_geometric_visibility_test;
    settings.global_seam_leveling = texture_settings.do_global_seam_leveling;
    settings.local_seam_leveling = texture_settings.do_local_seam_leveling;
    settings.hole_filling = texture_settings.do_hole_filling;
    settings.keep_unseen_faces = texture_settings.do_keep_unseen_faces;

    if (labeling_file.empty()) {
        std::cout << "View selection:" << std::endl;
        util::WallTimer rwtimer;

        tex::DataCosts data_costs(num_faces, texture_views.size());
        if (data_cost_file.empty()) {
            // std::cout << "- added - Calculating Data costs" << std::endl;
            tex::calculate_data_costs(mesh, &texture_views, settings, &data_costs, ev_mask, hidden_face_proportion);

            if (write_intermediate_results) {
                std::cout << "\tWriting data cost file... " << std::flush;
                tex::DataCosts::save_to_file(data_costs, out_prefix + "_data_costs.spt");
                std::cout << "done." << std::endl;
            }
        } else {
            std::cout << "\tLoading data cost file... " << std::flush;
            try {
                tex::DataCosts::load_from_file(data_cost_file, &data_costs);
            } catch (util::FileException e) {
                std::cout << "failed!" << std::endl;
                std::cerr << e.what() << std::endl;
                std::exit(EXIT_FAILURE);
            }
            std::cout << "done." << std::endl;
        }
        timer.measure("Calculating data costs");

        std::cout << "- added - Selecting Views" << std::endl;
        try {
            tex::view_selection(data_costs, &graph, settings);
        } catch (std::runtime_error& e) {
            std::cerr << "\tOptimization failed: " << e.what() << std::endl;
            std::exit(EXIT_FAILURE);
        }
        timer.measure("Running MRF optimization");
        std::cout << "\tTook: " << rwtimer.get_elapsed_sec() << "s" << std::endl;

        /* Write labeling to file. */
        if (write_intermediate_results) {
            std::vector<std::size_t> labeling(graph.num_nodes());
            for (std::size_t i = 0; i < graph.num_nodes(); ++i) {
                labeling[i] = graph.get_label(i);
            }
            vector_to_file(out_prefix + "_labeling.vec", labeling);
        }
    } else {
        std::cout << "Loading labeling from file... " << std::flush;

        /* Load labeling from file. */
        std::vector<std::size_t> labeling = vector_from_file<std::size_t>(labeling_file);
        if (labeling.size() != graph.num_nodes()) {
            std::cerr << "Wrong labeling file for this mesh/scene combination... aborting!" << std::endl;
            std::exit(EXIT_FAILURE);
        }

        /* Transfer labeling to graph. */
        for (std::size_t i = 0; i < labeling.size(); ++i) {
            const std::size_t label = labeling[i];
            if (label > texture_views.size()){
                std::cerr << "Wrong labeling file for this mesh/scene combination... aborting!" << std::endl;
                std::exit(EXIT_FAILURE);
            }
            graph.set_label(i, label);
        }

        std::cout << "done." << std::endl;
    }

    // generate full texture patchs
    tex::TexturePatches texture_patches;
    {
        /* Create texture patches and adjust them. */

        tex::VertexProjectionInfos vertex_projection_infos;
        std::cout << "Generating texture patches:" << std::endl;
        tex::generate_texture_patches(graph, mesh, mesh_info, &texture_views,
            settings, &vertex_projection_infos, &texture_patches);

        if (settings.global_seam_leveling) {
            std::cout << "Running global seam leveling:" << std::endl;
            tex::global_seam_leveling(graph, mesh, mesh_info, vertex_projection_infos, &texture_patches);
            timer.measure("Running global seam leveling");
        } else {
            ProgressCounter texture_patch_counter("Calculating validity masks for texture patches", texture_patches.size());
            #pragma omp parallel for schedule(dynamic)
            for (std::size_t i = 0; i < texture_patches.size(); ++i) {
                texture_patch_counter.progress<SIMPLE>();
                TexturePatch::Ptr texture_patch = texture_patches[i];
                std::vector<math::Vec3f> patch_adjust_values(texture_patch->get_faces().size() * 3, math::Vec3f(0.0f));
                texture_patch->adjust_colors(patch_adjust_values);
                texture_patch_counter.inc();
            }
            timer.measure("Calculating texture patch validity masks");
        }

        if (settings.local_seam_leveling) {
            std::cout << "Running local seam leveling:" << std::endl;
            tex::local_seam_leveling(graph, mesh, vertex_projection_infos, &texture_patches);
        }
        timer.measure("Running local seam leveling");
    }

    // Now loop, generating+saving subindexed meshes and atlas
    #pragma omp parallel for schedule(dynamic)
    for (int vi = 0; vi < sub_vert_masks.size(); ++vi) {
        std::cout << "\nFinalizing Sub-Model " << sub_names[vi] << " - " << vi+1 << " of " << sub_vert_masks.size() << std::endl;
        tex::TextureAtlases sub_texture_atlases;
        const std::vector<bool>& vertex_mask(sub_vert_masks[vi]);
        std::vector<bool> inverted_mask(vertex_mask.size());
        for (std::size_t i = 0; i < vertex_mask.size(); ++i)
            inverted_mask[i] = !vertex_mask[i];

        const std::string& sub_name(sub_names[vi]);
        std::vector<std::size_t> face_indices;
        // generate face reindex
        generate_face_reindex(vertex_mask, mesh->get_faces(), face_indices);
        // redo mesh
        mve::TriangleMesh::Ptr sub_mesh = mesh->duplicate();
        sub_mesh->delete_vertices_fix_faces(inverted_mask);

        if (sub_mesh->get_faces().size() == 0) {
            std::cout << "No Faces - skipping Sub-Model " << sub_name << std::endl;
            continue;
        }

        std::cout << "Model includes " << sub_mesh->get_faces().size()/3 << " of "
                  << mesh->get_faces().size()/3 << " faces." << std::endl;

        // redo_patches
        tex::TexturePatches sub_texture_patches;
        size_t patch_ct = 0;
        for(std::size_t i = 0; i < texture_patches.size(); ++i) {
            TexturePatch::Ptr new_patch = TexturePatch::create(texture_patches[i], face_indices);
            if (!new_patch->get_faces().empty()) {
                new_patch->set_label(patch_ct);
                sub_texture_patches.push_back(new_patch);
                patch_ct++;
            }
        }

        if (texture_patches.size() == 0) {
            std::cout << "No Texture Patches - skipping Sub-Model " << sub_name << std::endl;
            continue;
        }
         std::cout << "And " << sub_texture_patches.size() << " of "
          << texture_patches.size() << " patches." << std::endl;
        {
            /* Generate texture atlases. */
            std::cout << "Generating texture atlases:" << std::endl;
            // capped method is desireable, but resizes currently have some issues that make it worse.
            // tex::generate_capped_texture_atlas(&sub_texture_patches,
            //                                    settings,
            //                                    &sub_texture_atlases,
            //                                    atlas_size,
            //                                    mesh->get_vertices(),
            //                                    mesh->get_faces());
            tex::generate_texture_atlases(&sub_texture_patches,
                                               settings,
                                               &sub_texture_atlases,
                                               mesh->get_vertices(),
                                               mesh->get_faces());
        }

        /* Create and write out obj model. */
        {
            std::cout << "Building objmodel:" << std::endl;
            tex::Model sub_model;
            tex::build_model(sub_mesh, sub_texture_atlases, &sub_model);
            timer.measure("Building OBJ model");

            std::cout << "\tSaving model to " << out_prefix+sub_name << "... " << std::flush;
            tex::Model::save(sub_model, out_prefix+sub_name);
            std::cout << "done." << std::endl;
            timer.measure("Saving");
        }

        timer.measure("Total");
    }
    std::cout << "Whole texturing procedure took: " << wtimer.get_elapsed_sec() << "s" << std::endl;

    /* Remove temporary files. */
    for (util::fs::File const & file : util::fs::Directory(tmp_dir)) {
        util::fs::unlink(util::fs::join_path(file.path, file.name).c_str());
    }
    util::fs::rmdir(tmp_dir.c_str());

}

void generate_vertex_reindex(const std::vector<bool>& mask, std::vector<std::size_t>& new_indices) {
    new_indices.resize(mask.size());
    std::size_t ct = 0;
    for (std::size_t i = 0; i < new_indices.size(); ++i) {
        if (mask[i]) {
            new_indices[i] = ct;
            ++ct;
        } else {
            new_indices[i] = std::numeric_limits<std::size_t>::max();
        }
    }
}

bool is_valid_tri(std::size_t i, const std::vector<bool>& mask, const std::vector<unsigned int>& old_faces) {
    return mask[old_faces[i*3]] && mask[old_faces[i*3+1]] && mask[old_faces[i*3+2]];
}


/**
 * @brief Strange reindexing to match the swap-based MVE reduction
 */
void generate_face_reindex(const std::vector<bool>& mask,
                           const std::vector<unsigned int>& old_faces,
                           std::vector<std::size_t>& new_indices) {
    new_indices.resize(old_faces.size()/3);
    std::size_t front = 0;
    std::size_t back = new_indices.size() -1;
    while (front < back) {
        if (is_valid_tri(front, mask, old_faces)) {
            new_indices[front] = front;
            ++front;
        } else {
            while (front < back && !is_valid_tri(back, mask, old_faces)) {
                new_indices[back] = std::numeric_limits<std::size_t>::max();
                --back;
            }
            if (back > front && is_valid_tri(back, mask, old_faces)) {
                new_indices[back] = front;
                back--;
                front++;
            }
        }
    }
}




}  // namespace MvsTexturing
