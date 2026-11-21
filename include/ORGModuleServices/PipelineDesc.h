#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <rhi.h>

// Graphics pipelines described by value. The host builds them. Moved here from ORG, which builds no pipelines.
namespace org::services {

// A shader of a pipeline. An empty target takes the stage's default (vs_6_6, ps_6_6, ...).
struct PipelineShaderSource {
	std::wstring path;
	std::wstring entry;
	std::wstring target;
};

/// @brief One vertex attribute of a pipeline's input layout. It owns its semantic name, unlike rhi::InputAttributeDesc.
struct PipelineInputAttribute {
	uint32_t binding = 0;                  ///< The vertex buffer slot it reads from.
	uint32_t offset = rhi::APPEND_ALIGNED; ///< Byte offset in the vertex. APPEND_ALIGNED places it after the previous one.
	rhi::Format format = rhi::Format::Unknown;
	std::string semanticName;              ///< D3D12 matches the vertex shader's inputs by semantic, such as "TEXCOORD".
	uint32_t semanticIndex = 0;
	/// Vulkan matches inputs by location. Unset locations count up in declaration order. That is the order DXC gives the
	/// vertex shader's inputs.
	uint32_t location = 0xFFFFFFFFu;
};

/// @brief The vertex buffers a pipeline reads, and how their bytes map to vertex shader inputs.
///
/// A pipeline with an input layout draws from vertex buffers bound with CommandList::SetVertexBuffers. Without one, the
/// vertex shader pulls its own vertices. On D3D12 the pipeline's layout must allow the input assembler
/// (rhi::PF_AllowInputAssembler).
struct PipelineInputLayout {
	std::vector<rhi::InputBindingDesc> bindings;
	std::vector<PipelineInputAttribute> attributes;
};

// An absent state is not given to the device at all, which leaves that state to the backend's defaults. Topology is the
// exception: backends disagree about an omitted one, so it is always given (TriangleList unless set).
struct GraphicsPipelineDesc {
	std::optional<PipelineShaderSource> vertex;
	std::optional<PipelineShaderSource> pixel;
	std::string name;
	std::optional<rhi::RasterState> raster;
	std::optional<rhi::BlendState> blend;
	std::optional<rhi::DepthStencilState> depth;
	std::optional<rhi::RenderTargets> renderTargets;
	std::optional<rhi::Format> depthFormat;
	std::optional<rhi::SampleDesc> sample;
	std::optional<rhi::PrimitiveTopology> topology;
	/// When absent, the vertex shader pulls its own vertices.
	std::optional<PipelineInputLayout> inputLayout;
};

} // namespace org::services
