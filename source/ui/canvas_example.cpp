#include "canvas_example.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/solution_file.hpp>

namespace visionpro::ui {
namespace {

using namespace core;
using namespace workflow;

template<UuidIdentity Id>
Id id(std::string_view suffix)
{
    return std::get<Id>(IdFactory::parse<Id>("35110000-0000-4000-8000-0000000000" + std::string(suffix)));
}

template<SchemaIdentity Id>
Id key(std::string_view value)
{
    return std::get<Id>(SchemaIdFactory::parse<Id>(value));
}

Node add_node(NodeId id, std::string name)
{
    return {id, std::move(name), ToolConfiguration{key<ToolTypeId>("visionpro.sim.add"), 1},
        {{key<InputId>("left"), {ConfigValue::integer(1)}}, {key<InputId>("right"), {ConfigValue::integer(2)}}}, {}};
}

} // namespace

workflow::Solution canvas_example()
{
    const auto workflow_id = id<WorkflowId>("01");
    const auto route = id<NodeId>("01");
    const auto left = id<NodeId>("02");
    const auto right = id<NodeId>("03");
    const auto reference = id<NodeId>("04");
    const auto merge = id<NodeId>("05");
    const auto group = id<NodeId>("06");
    const auto finish = id<NodeId>("07");
    const auto prepare = id<NodeId>("08");
    const auto review = id<NodeId>("09");
    const auto done = key<ControlOutputId>("done");
    auto merge_node = add_node(merge, "Merge");
    merge_node.bindings.front().sources = {OutputReference{workflow_id, left, key<OutputId>("value"), {}}};
    Workflow flow{
        workflow_id, "Inspection example", {key<SchemaId>("visionpro.example.inspection"), 1, {}, {}, {}},
        {
            {route, "Route", ToolConfiguration{key<ToolTypeId>("visionpro.sim.select"), 1},
                {{key<InputId>("mask"), {ConfigValue::integer(3)}}}, {}},
            add_node(left, "Left check"), add_node(right, "Right check"), add_node(reference, "Reference"),
            std::move(merge_node),
            {group, "Review", GroupConfiguration{
                {key<SchemaId>("visionpro.example.review"), 1, {}, {}, {{done, "Done"}}},
                {prepare}, {}, {{done, review, std::nullopt}}}, {}, {}},
            add_node(finish, "Finish"), add_node(prepare, "Prepare"), add_node(review, "Review check"),
        },
        {
            {id<ControlLinkId>("01"), route, key<ControlOutputId>("left"), left},
            {id<ControlLinkId>("02"), route, key<ControlOutputId>("left"), reference},
            {id<ControlLinkId>("03"), route, key<ControlOutputId>("right"), right},
            {id<ControlLinkId>("04"), left, std::nullopt, merge},
            {id<ControlLinkId>("05"), right, std::nullopt, merge},
            {id<ControlLinkId>("06"), reference, std::nullopt, merge},
            {id<ControlLinkId>("07"), merge, std::nullopt, group},
            {id<ControlLinkId>("08"), group, done, finish},
            {id<ControlLinkId>("09"), prepare, std::nullopt, review},
        },
        {{group, prepare}, {group, review}}, {route}, {}, true, {}
    };
    return {id<SolutionId>("01"), "Canvas example", infrastructure::solution_format_version, 1,
        {std::move(flow)}, {}, {}, {}, {}, {{workflow_id, {
            {route, 0, 155}, {left, 245, 0}, {right, 245, 155}, {reference, 245, 310},
            {merge, 475, 155}, {group, 690, 155}, {finish, 940, 155},
            {prepare, 0, 0}, {review, 260, 0}
        }}}};
}

} // namespace visionpro::ui
