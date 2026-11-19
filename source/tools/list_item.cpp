#include "list_item.hpp"

#include <visionpro/core/schema_id_factory.hpp>

#include <algorithm>

namespace visionpro::tools {
namespace {
using namespace core;

template<SchemaIdentity Id> Id key(std::string_view name)
{ return std::get<Id>(SchemaIdFactory::parse<Id>(name)); }

class ListItem final : public ToolInstance {
public:
    explicit ListItem(const ToolExecutionConfiguration& config) : ToolInstance(config) {}

private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion) override
    {
        const auto& call = context->data();
        const auto by_name = [](const FixedToolInput& input) { return input.id.text(); };
        const auto& list = *std::ranges::find(call.inputs, "list", by_name)->value.elements();
        const auto index_input = std::ranges::find(call.inputs, "index", by_name);
        const auto index = *index_input->value.as_int64();
        if (index < 0 || static_cast<std::uint64_t>(index) >= list.size()) {
            DiagnosticLocation where;
            where.input = index_input->id;
            return Diagnostic{ErrorCode::IndexOutOfRange, Severity::Error, where, "List index is out of range."};
        }
        return InvocationStart{ToolCompletion{call.identity, ToolCompletionStatus::Succeeded, ToolJudgement::NotEvaluated,
            {{key<OutputId>("item"), list[static_cast<std::size_t>(index)]}}, {}}};
    }
};

class ListItemFactory final : public ToolInstanceFactory {
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& config) override
    { return std::shared_ptr<ToolInstance>{std::make_shared<ListItem>(config)}; }
};
} // namespace

core::ToolRegistration list_item_registration(std::string_view id, std::string name, const core::DataType& element)
{
    const auto schema = key<SchemaId>(id);
    BindingPolicy binding;
    binding.allowed_sources = {BindingSourceKind::Constant, BindingSourceKind::RunOutput, BindingSourceKind::PublicInput};
    ToolDescriptor tool{key<ToolTypeId>(id), std::move(name), 1, {schema, 1, {}}, {},
        {{key<InputId>("list"), "List", DataType::list(element), true, {}, {}, binding, {}},
         {key<InputId>("index"), "Index", DataType::integer(), true, {}, {}, binding, {}}},
        {{key<OutputId>("item"), "Selected item", element, {}}}, {}, {}, {}, {}, {}, {}, {}};
    tool.execution.retry = ToolRetry::Idempotent;
    return {std::move(tool), {schema, 1, 1, {}}, std::make_shared<ListItemFactory>()};
}
} // namespace visionpro::tools
