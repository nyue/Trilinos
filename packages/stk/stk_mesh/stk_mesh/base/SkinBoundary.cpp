// Copyright (c) 2013, Sandia Corporation.
// Under the terms of Contract DE-AC04-94AL85000 with Sandia Corporation,
// the U.S. Government retains certain rights in this software.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//
//     * Neither the name of Sandia Corporation nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//

#include <stk_mesh/base/BulkData.hpp>
#include <stk_mesh/base/MetaData.hpp>
#include <stk_mesh/base/Part.hpp>
#include <stk_mesh/base/Types.hpp>
#include <stk_mesh/baseImpl/elementGraph/ElemElemGraph.hpp>
#include <stk_mesh/baseImpl/elementGraph/SkinBoundaryErrorReporter.hpp>
#include <stk_mesh/base/GetEntities.hpp>
#include <stk_util/parallel/ParallelReduceBool.hpp>
#include <stk_util/util/SortAndUnique.hpp>
#include <stk_mesh/baseImpl/EquivalentEntityBlocks.hpp>
#include <stk_mesh/base/SideSetEntry.hpp>
#include <stk_mesh/base/FaceCreator.hpp>
#include <stk_mesh/base/SkinMeshUtil.hpp>
#include <stk_mesh/base/CreateEdges.hpp>

namespace stk
{
namespace mesh
{


///////////////////////////////////////////////////////////////////////////

void create_exposed_block_boundary_sides(BulkData &bulkData, const Selector& blocksToSkin, const stk::mesh::PartVector& partToPutSidesInto, stk::mesh::Selector* air)
{
    bulkData.initialize_face_adjacent_element_graph();
    ElemElemGraph& elemElemGraph = bulkData.get_face_adjacent_element_graph();
    SkinMeshUtil skinMesh(elemElemGraph, blocksToSkin, air);
    std::vector<SideSetEntry> skinnedSideSet = skinMesh.extract_skinned_sideset();

    FaceCreator(bulkData, elemElemGraph).create_side_entities_given_sideset(skinnedSideSet, partToPutSidesInto);
}

void create_exposed_block_boundary_sides(BulkData &bulkData, const Selector& blocksToSkin, const stk::mesh::PartVector& partToPutSidesInto)
{
    stk::mesh::Selector *air = nullptr;
    create_exposed_block_boundary_sides(bulkData, blocksToSkin, partToPutSidesInto, air);
}

void create_interior_block_boundary_sides(stk::mesh::BulkData &bulkData, const stk::mesh::Selector &blocksToConsider, const stk::mesh::PartVector& partToPutSidesInto)
{
    bulkData.initialize_face_adjacent_element_graph();
    ElemElemGraph& elemElemGraph = bulkData.get_face_adjacent_element_graph();
    SkinMeshUtil skinMesh(elemElemGraph, blocksToConsider);
    std::vector<SideSetEntry> skinnedSideSet = skinMesh.extract_interior_sideset();

    FaceCreator(bulkData, elemElemGraph).create_side_entities_given_sideset(skinnedSideSet, partToPutSidesInto);
}

void create_all_sides(stk::mesh::BulkData &bulkData, const stk::mesh::Selector &blocksToConsider, const stk::mesh::PartVector& partToPutSidesInto, bool connect_faces_to_edges)
{
    bulkData.initialize_face_adjacent_element_graph();
    ElemElemGraph& graph = bulkData.get_face_adjacent_element_graph();
    SkinMeshUtil skinMesh(graph, blocksToConsider);
    std::vector<SideSetEntry> sideSet = skinMesh.extract_all_sides_sideset();

    FaceCreator(bulkData, graph).create_side_entities_given_sideset(sideSet, partToPutSidesInto);

    impl::edge_map_type edge_map;
    if (connect_faces_to_edges) {
        //populate the edge_map with existing edges

        BucketVector const & edge_buckets = bulkData.buckets(stk::topology::EDGE_RANK);

        for (size_t i=0, ie=edge_buckets.size(); i<ie; ++i) {
            Bucket &b = *edge_buckets[i];

            const unsigned num_nodes = b.topology().num_nodes();
            EntityVector edge_nodes(num_nodes);

            for (size_t j=0, je=b.size(); j<je; ++j) {
                Entity edge = b[j];
                Entity const *nodes_rel = b.begin_nodes(j);

                for (unsigned n=0; n<num_nodes; ++n) {
                    edge_nodes[n] = nodes_rel[n];
                }

                edge_map[edge_nodes] = edge;
            }
        }

        bulkData.modification_begin();
        // connect pre-existing edges to new faces
        impl::connect_faces_to_edges(bulkData, blocksToConsider, edge_map);
        bulkData.modification_end();
    }
}

////////////////////////////////////////////////////////////////////////////////////////

Entity get_side_entity_from_ordinal(const std::vector<Entity> &sides, ConnectivityOrdinal const * ordinals, ConnectivityOrdinal requestedOrdinal)
{
    for(unsigned i = 0; i<sides.size(); ++i)
    {
        if(ordinals[i] == requestedOrdinal)
            return sides[i];
    }

    return Entity();
}

Entity get_side_entity_for_element_side_pair(BulkData &bulkData, const SideSetEntry &facet)
{
    const Entity * sides = bulkData.begin(facet.element, bulkData.mesh_meta_data().side_rank());
    ConnectivityOrdinal const * ordinals = bulkData.begin_ordinals(facet.element, bulkData.mesh_meta_data().side_rank());
    std::vector<Entity> sideVector(sides, sides+bulkData.num_sides(facet.element));
    return get_side_entity_from_ordinal(sideVector, ordinals, facet.side);
}

stk::mesh::EntityVector get_locally_owned_skinned_sides(BulkData &bulkData, const Part& skinnedPart)
{
    stk::mesh::EntityVector skinnedSides;
    stk::mesh::get_selected_entities(skinnedPart & bulkData.mesh_meta_data().locally_owned_part(), bulkData.buckets(bulkData.mesh_meta_data().side_rank()), skinnedSides);
    return skinnedSides;
}

bool is_sideset_equivalent_to_skin(BulkData &bulkData, stk::mesh::EntityVector &sidesetSides, const Part& skinnedPart, impl::SkinBoundaryErrorReporter &reporter)
{
    stk::mesh::EntityVector skinnedSides = get_locally_owned_skinned_sides(bulkData, skinnedPart);
    stk::util::sort_and_unique(sidesetSides);
    stk::util::sort_and_unique(skinnedSides);
    bool result =  stk::is_true_on_all_procs(bulkData.parallel(), sidesetSides == skinnedSides);

    if(!result)
        reporter.report(skinnedSides, sidesetSides, skinnedPart);

    return result;
}

void add_locally_owned_side_from_element_side_pair(BulkData &bulkData, const SideSetEntry &facet, stk::mesh::EntityVector &sidesetSides, impl::SkinBoundaryErrorReporter &reporter)
{
    Entity side = get_side_entity_for_element_side_pair(bulkData, facet);
    if(bulkData.is_valid(side) && bulkData.bucket(side).owned())
    {
        sidesetSides.push_back(side);
        reporter.add_entry(side, facet);
    }
}

stk::mesh::EntityVector get_locally_owned_sides_from_sideset(BulkData &bulkData, std::vector<SideSetEntry> &skinnedSideSet, impl::SkinBoundaryErrorReporter &reporter)
{
    stk::mesh::EntityVector sidesetSides;

    for(const SideSetEntry &facet : skinnedSideSet)
        add_locally_owned_side_from_element_side_pair(bulkData, facet, sidesetSides, reporter);

    return sidesetSides;
}


bool check_exposed_block_boundary_sides(BulkData &bulkData, const Selector& skinnedBlock, Part& skinnedPart, std::ostream &stream)
{
    bulkData.initialize_face_adjacent_element_graph();
    ElemElemGraph& elemElemGraph = bulkData.get_face_adjacent_element_graph();
    SkinMeshUtil skinMesh(elemElemGraph, skinnedBlock);
    std::vector<SideSetEntry> skinnedSideSet = skinMesh.extract_skinned_sideset();
    impl::SkinBoundaryErrorReporter reporter(stream, bulkData);

    stk::mesh::EntityVector sidesetSides = get_locally_owned_sides_from_sideset(bulkData, skinnedSideSet, reporter);
    return is_sideset_equivalent_to_skin(bulkData, sidesetSides, skinnedPart, reporter);
}

bool check_exposed_block_boundary_sides(BulkData &bulkData, const Selector& skinnedBlock, Part& skinnedPart)
{
    return check_exposed_block_boundary_sides(bulkData, skinnedBlock, skinnedPart, std::cerr);
}

bool check_interior_block_boundary_sides(stk::mesh::BulkData &bulkData, const stk::mesh::Selector &skinnedBlock, stk::mesh::Part &skinnedPart, std::ostream &stream)
{
    bulkData.initialize_face_adjacent_element_graph();
    ElemElemGraph& graph = bulkData.get_face_adjacent_element_graph();
    SkinMeshUtil skinMesh(graph, skinnedBlock);
    std::vector<SideSetEntry> skinnedSideSet = skinMesh.extract_interior_sideset();
    impl::SkinBoundaryErrorReporter reporter(stream, bulkData);

    stk::mesh::EntityVector sidesetSides = stk::mesh::get_locally_owned_sides_from_sideset(bulkData, skinnedSideSet, reporter);
    return stk::mesh::is_sideset_equivalent_to_skin(bulkData, sidesetSides, skinnedPart, reporter);
}

bool check_interior_block_boundary_sides(stk::mesh::BulkData &bulkData, const stk::mesh::Selector &skinnedBlock, stk::mesh::Part &skinnedPart)
{
    return check_interior_block_boundary_sides(bulkData, skinnedBlock, skinnedPart, std::cerr);
}

bool check_all_sides(stk::mesh::BulkData &bulkData, const stk::mesh::Selector &skinnedBlock, stk::mesh::Part &skinnedPart, std::ostream &stream)
{
    bulkData.initialize_face_adjacent_element_graph();
    ElemElemGraph& graph = bulkData.get_face_adjacent_element_graph();
    SkinMeshUtil skinMesh(graph, skinnedBlock);
    std::vector<SideSetEntry> skinnedSideSet = skinMesh.extract_all_sides_sideset();
    impl::SkinBoundaryErrorReporter reporter(stream, bulkData);

    stk::mesh::EntityVector sidesetSides = stk::mesh::get_locally_owned_sides_from_sideset(bulkData, skinnedSideSet, reporter);
    return stk::mesh::is_sideset_equivalent_to_skin(bulkData, sidesetSides, skinnedPart, reporter);
}

bool check_all_sides(stk::mesh::BulkData &bulkData, const stk::mesh::Selector &skinnedBlock, stk::mesh::Part &skinnedPart)
{
    return check_all_sides(bulkData, skinnedBlock, skinnedPart, std::cerr);
}

} // namespace mesh
} // namespace stk
