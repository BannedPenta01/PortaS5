#include "graphics/presentation/window/fsrUpscaler.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "fsr1_shaders/fsr1_easu_spv.h"
#include "fsr1_shaders/fsr1_rcas_spv.h"

#define A_CPU 1
#include "ffx_a.h"
#include "ffx_fsr1.h"

#include <array>

namespace Libs::Graphics {

namespace {

constexpr uint32_t kEasuLocalSizeX = 64;

vk::ImageMemoryBarrier2 GeneralBarrier(
    vk::PipelineStageFlagBits2 src_stage, vk::AccessFlags2 src_access,
    vk::ImageLayout old_layout, vk::PipelineStageFlagBits2 dst_stage,
    vk::AccessFlags2 dst_access, vk::ImageLayout new_layout, vk::Image image) {
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask    = src_stage;
	barrier.dstStageMask    = dst_stage;
	barrier.srcAccessMask   = src_access;
	barrier.dstAccessMask   = dst_access;
	barrier.oldLayout       = old_layout;
	barrier.newLayout       = new_layout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image               = image;
	barrier.subresourceRange    = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	return barrier;
}

} // namespace

bool FsrUpscaler::Initialize(GraphicContext& graphics, vk::Format target_format) {
	EXIT_IF(m_enabled);
	EXIT_IF(graphics.device == nullptr);

	// EASU and RCAS both write storage images; the target format
	// must support that on optimal tiling.
	const auto features = graphics.GetFormatProperties(target_format).optimalTilingFeatures;
	if (!static_cast<bool>(features & vk::FormatFeatureFlagBits::eStorageImage)) {
		LOGF("FSR 1: swapchain format lacks storage-image support: format=%d\n",
		     static_cast<int>(target_format));
		return false;
	}

	m_format = target_format;

	vk::SamplerCreateInfo sampler {};
	sampler.magFilter    = vk::Filter::eLinear;
	sampler.minFilter    = vk::Filter::eLinear;
	sampler.addressModeU = vk::SamplerAddressMode::eClampToEdge;
	sampler.addressModeV = vk::SamplerAddressMode::eClampToEdge;
	sampler.addressModeW = vk::SamplerAddressMode::eClampToEdge;
	RequireVulkanSuccess(graphics.device.createSampler(&sampler, nullptr, &m_sampler),
	                     "create FSR sampler");

	// Push-descriptor set layout shared by both passes:
	// binding 0 = combined image sampler (input), binding 1 = storage image.
	const std::array<vk::DescriptorSetLayoutBinding, 2> bindings {{
	    {0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eCompute},
	    {1, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
	}};
	vk::DescriptorSetLayoutCreateInfo descriptors {};
	descriptors.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	descriptors.bindingCount = static_cast<uint32_t>(bindings.size());
	descriptors.pBindings    = bindings.data();
	RequireVulkanSuccess(
	    graphics.device.createDescriptorSetLayout(&descriptors, nullptr, &m_descriptor_set),
	    "create FSR descriptor set layout");

	const auto create_pipeline_layout = [&](vk::PushConstantRange constants,
	                                        vk::PipelineLayout& out) {
		vk::PipelineLayoutCreateInfo layout {};
		layout.setLayoutCount         = 1;
		layout.pSetLayouts            = &m_descriptor_set;
		layout.pushConstantRangeCount = 1;
		layout.pPushConstantRanges    = &constants;
		RequireVulkanSuccess(graphics.device.createPipelineLayout(&layout, nullptr, &out),
		                     "create FSR pipeline layout");
	};
	const vk::PushConstantRange easu_constants {vk::ShaderStageFlagBits::eCompute, 0,
	                                            4 * sizeof(uint32_t) * 4};
	const vk::PushConstantRange rcas_constants {vk::ShaderStageFlagBits::eCompute, 0,
	                                            sizeof(uint32_t) * 4};
	create_pipeline_layout(easu_constants, m_easu_layout);
	create_pipeline_layout(rcas_constants, m_rcas_layout);

	const auto easu_module   = CompileSPV(FSR1_EASU_SPV, graphics.device);
	const auto rcas_module   = CompileSPV(FSR1_RCAS_SPV, graphics.device);
	vk::ComputePipelineCreateInfo pipelines[2] {};
	pipelines[0].stage.sType  = vk::StructureType::ePipelineShaderStageCreateInfo;
	pipelines[0].stage.stage  = vk::ShaderStageFlagBits::eCompute;
	pipelines[0].stage.module = easu_module;
	pipelines[0].stage.pName  = "main";
	pipelines[0].layout       = m_easu_layout;
	pipelines[1].stage.sType  = vk::StructureType::ePipelineShaderStageCreateInfo;
	pipelines[1].stage.stage  = vk::ShaderStageFlagBits::eCompute;
	pipelines[1].stage.module = rcas_module;
	pipelines[1].stage.pName  = "main";
	pipelines[1].layout       = m_rcas_layout;
	vk::Pipeline created[2] = {nullptr, nullptr};
	RequireVulkanSuccess(
	    graphics.device.createComputePipelines(nullptr, 2, pipelines, nullptr, created),
	    "create FSR compute pipelines");
	m_easu_pipeline = created[0];
	m_rcas_pipeline = created[1];
	graphics.device.destroyShaderModule(easu_module, nullptr);
	graphics.device.destroyShaderModule(rcas_module, nullptr);

	m_enabled = true;
	if (::std::getenv("KYTY_FPS_LOG") != nullptr) {
		::std::printf("FSR: enabled, target format=%d\n",
		               static_cast<int>(target_format));
		::std::fflush(stdout);
	}
	return true;
}

void FsrUpscaler::Shutdown(GraphicContext& graphics) {
	if (!m_enabled) {
		return;
	}
	if (m_intermediate_view != nullptr) {
		graphics.device.destroyImageView(m_intermediate_view, nullptr);
		m_intermediate_view = nullptr;
	}
	if (m_intermediate.image != nullptr) {
		graphics.DeleteImage(m_intermediate);
	}
	m_intermediate.state = {};
	if (m_easu_pipeline != nullptr) {
		graphics.device.destroyPipeline(m_easu_pipeline, nullptr);
		m_easu_pipeline = nullptr;
	}
	if (m_rcas_pipeline != nullptr) {
		graphics.device.destroyPipeline(m_rcas_pipeline, nullptr);
		m_rcas_pipeline = nullptr;
	}
	if (m_easu_layout != nullptr) {
		graphics.device.destroyPipelineLayout(m_easu_layout, nullptr);
		m_easu_layout = nullptr;
	}
	if (m_rcas_layout != nullptr) {
		graphics.device.destroyPipelineLayout(m_rcas_layout, nullptr);
		m_rcas_layout = nullptr;
	}
	if (m_descriptor_set != nullptr) {
		graphics.device.destroyDescriptorSetLayout(m_descriptor_set, nullptr);
		m_descriptor_set = nullptr;
	}
	if (m_sampler != nullptr) {
		graphics.device.destroySampler(m_sampler, nullptr);
		m_sampler = nullptr;
	}
	m_enabled = false;
	m_format  = vk::Format::eUndefined;
}

bool FsrUpscaler::Prepare(GraphicContext& graphics, vk::Extent2D target_extent,
                          vk::Format target_format) {
	EXIT_IF(!m_enabled);
	const bool compatible = m_intermediate.image != nullptr &&
	                        m_intermediate.extent.width == target_extent.width &&
	                        m_intermediate.extent.height == target_extent.height &&
	                        m_intermediate.format == target_format;
	if (compatible) {
		return true;
	}
	EXIT_IF(m_intermediate.image != nullptr); // extent changes only via swapchain recreation
	vk::ImageCreateInfo create {};
	create.sType         = vk::StructureType::eImageCreateInfo;
	create.imageType     = vk::ImageType::e2D;
	create.extent        = vk::Extent3D {target_extent.width, target_extent.height, 1};
	create.mipLevels     = 1;
	create.arrayLayers   = 1;
	create.format        = target_format;
	create.tiling        = vk::ImageTiling::eOptimal;
	create.initialLayout = vk::ImageLayout::eUndefined;
	create.usage         = vk::ImageUsageFlagBits::eStorage |
	                       vk::ImageUsageFlagBits::eSampled;
	create.sharingMode = vk::SharingMode::eExclusive;
	create.samples     = vk::SampleCountFlagBits::e1;
	if (!graphics.CreateImage(create, m_intermediate)) {
		LOGF("FSR 1: failed to allocate intermediate image, extent=%ux%u\n",
		     target_extent.width, target_extent.height);
		return false;
	}
	vk::ImageViewCreateInfo view {};
	view.sType                           = vk::StructureType::eImageViewCreateInfo;
	view.image                           = m_intermediate.image;
	view.viewType                        = vk::ImageViewType::e2D;
	view.format                          = target_format;
	view.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	view.subresourceRange.baseMipLevel   = 0;
	view.subresourceRange.levelCount     = 1;
	view.subresourceRange.baseArrayLayer = 0;
	view.subresourceRange.layerCount     = 1;
	RequireVulkanSuccess(graphics.device.createImageView(&view, nullptr, &m_intermediate_view),
	                     "create FSR intermediate image view");
	return true;
}

void FsrUpscaler::Record(vk::CommandBuffer command, const VulkanImage& source,
                         vk::ImageView source_view, vk::Image target,
                         vk::ImageView target_view) {
	EXIT_IF(!m_enabled);
	const auto extent = m_intermediate.extent;

	// Intermediate: first use transitions Undefined -> General.
	if (m_intermediate.state.layout != vk::ImageLayout::eGeneral) {
		const auto barrier = GeneralBarrier(
		    vk::PipelineStageFlagBits2::eNone, vk::AccessFlags2 {},
		    vk::ImageLayout::eUndefined, vk::PipelineStageFlagBits2::eComputeShader,
		    vk::AccessFlagBits2::eShaderWrite, vk::ImageLayout::eGeneral,
		    m_intermediate.image);
		vk::DependencyInfo dependency {};
		dependency.imageMemoryBarrierCount = 1;
		dependency.pImageMemoryBarriers    = &barrier;
		command.pipelineBarrier2(dependency);
		m_intermediate.state = {vk::PipelineStageFlagBits2::eComputeShader,
		                        vk::AccessFlagBits2::eShaderWrite,
		                        vk::ImageLayout::eGeneral};
	}

	// Target (swapchain image): freshly acquired, transition to
	// General for the RCAS store. Contents are fully overwritten.
	{
		const auto barrier = GeneralBarrier(
		    vk::PipelineStageFlagBits2::eNone, vk::AccessFlags2 {},
		    vk::ImageLayout::eUndefined, vk::PipelineStageFlagBits2::eComputeShader,
		    vk::AccessFlagBits2::eShaderWrite, vk::ImageLayout::eGeneral, target);
		vk::DependencyInfo dependency {};
		dependency.imageMemoryBarrierCount = 1;
		dependency.pImageMemoryBarriers    = &barrier;
		command.pipelineBarrier2(dependency);
	}

	// EASU: source -> intermediate.
	{
		AU1 con0[4], con1[4], con2[4], con3[4];
		FsrEasuCon(con0, con1, con2, con3,
		           AF1_(source.extent.width), AF1_(source.extent.height),
		           AF1_(source.extent.width), AF1_(source.extent.height),
		           AF1_(extent.width), AF1_(extent.height));
		const vk::DescriptorImageInfo input {m_sampler, source_view,
		                                     vk::ImageLayout::eShaderReadOnlyOptimal};
		const vk::DescriptorImageInfo output {nullptr, m_intermediate_view,
		                                      vk::ImageLayout::eGeneral};
		vk::WriteDescriptorSet write_input {};
		write_input.dstBinding      = 0;
		write_input.descriptorCount = 1;
		write_input.descriptorType  = vk::DescriptorType::eCombinedImageSampler;
		write_input.pImageInfo      = &input;
		vk::WriteDescriptorSet write_output {};
		write_output.dstBinding      = 1;
		write_output.descriptorCount = 1;
		write_output.descriptorType  = vk::DescriptorType::eStorageImage;
		write_output.pImageInfo      = &output;
		const std::array<vk::WriteDescriptorSet, 2> set {write_input, write_output};
		command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_easu_layout,
		                             0, static_cast<uint32_t>(set.size()), set.data());
		const uint32_t constants[16] = {con0[0], con0[1], con0[2], con0[3],
		                                con1[0], con1[1], con1[2], con1[3],
		                                con2[0], con2[1], con2[2], con2[3],
		                                con3[0], con3[1], con3[2], con3[3]};
		command.pushConstants(m_easu_layout, vk::ShaderStageFlagBits::eCompute, 0,
		                      sizeof(constants), constants);
		command.bindPipeline(vk::PipelineBindPoint::eCompute, m_easu_pipeline);
		command.dispatch((extent.width + kEasuLocalSizeX - 1) / kEasuLocalSizeX,
		                 extent.height, 1);
	}

	// Intermediate: shader write -> shader read for RCAS.
	{
		const auto barrier = GeneralBarrier(
		    vk::PipelineStageFlagBits2::eComputeShader,
		    vk::AccessFlagBits2::eShaderWrite, vk::ImageLayout::eGeneral,
		    vk::PipelineStageFlagBits2::eComputeShader,
		    vk::AccessFlagBits2::eShaderRead, vk::ImageLayout::eGeneral,
		    m_intermediate.image);
		vk::DependencyInfo dependency {};
		dependency.imageMemoryBarrierCount = 1;
		dependency.pImageMemoryBarriers    = &barrier;
		command.pipelineBarrier2(dependency);
		m_intermediate.state = {vk::PipelineStageFlagBits2::eComputeShader,
		                        vk::AccessFlagBits2::eShaderRead,
		                        vk::ImageLayout::eGeneral};
	}

	// RCAS: intermediate -> target.
	{
		AU1 con[4];
		FsrRcasCon(con, AF1_(0.0));
		const vk::DescriptorImageInfo input {m_sampler, m_intermediate_view,
		                                     vk::ImageLayout::eShaderReadOnlyOptimal};
		const vk::DescriptorImageInfo output {nullptr, target_view,
		                                      vk::ImageLayout::eGeneral};
		vk::WriteDescriptorSet write_input {};
		write_input.dstBinding      = 0;
		write_input.descriptorCount = 1;
		write_input.descriptorType  = vk::DescriptorType::eCombinedImageSampler;
		write_input.pImageInfo      = &input;
		vk::WriteDescriptorSet write_output {};
		write_output.dstBinding      = 1;
		write_output.descriptorCount = 1;
		write_output.descriptorType  = vk::DescriptorType::eStorageImage;
		write_output.pImageInfo      = &output;
		const std::array<vk::WriteDescriptorSet, 2> set {write_input, write_output};
		command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_rcas_layout,
		                             0, static_cast<uint32_t>(set.size()), set.data());
		command.pushConstants(m_rcas_layout, vk::ShaderStageFlagBits::eCompute, 0,
		                      sizeof(con), con);
		command.bindPipeline(vk::PipelineBindPoint::eCompute, m_rcas_pipeline);
		command.dispatch((extent.width + kEasuLocalSizeX - 1) / kEasuLocalSizeX,
		                 extent.height, 1);
	}
}

} // namespace Libs::Graphics
