#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_FSRUPSCALER_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_FSRUPSCALER_H_

#include "graphics/host_gpu/graphicContext.h"

#include <vulkan/vulkan.hpp>

namespace Libs::Graphics {

// FSR 1 (EASU + RCAS) presentation upscaler, AMD MIT.
// Engages only when the guest frame is strictly smaller than the
// swapchain, replacing the bilinear blit with an edge-adaptive
// upscale followed by contrast-adaptive sharpening. The guest
// itself still renders at its native resolution; no guest work is
// skipped or reduced.
class FsrUpscaler {
public:
	FsrUpscaler()  = default;
	~FsrUpscaler() = default;
	KYTY_CLASS_NO_COPY(FsrUpscaler);

	[[nodiscard]] bool Enabled() const noexcept { return m_enabled; }

	// One-time pipeline setup. Returns false when the device or the
	// target format cannot host storage-image upscaling; the caller
	// then keeps the bilinear blit path.
	bool Initialize(GraphicContext& graphics, vk::Format target_format);

	// Releases every Vulkan object. The queue must be idle.
	void Shutdown(GraphicContext& graphics);

	// Ensures the intermediate image matches the target extent and
	// format. Returns false when allocation fails.
	bool Prepare(GraphicContext& graphics, vk::Extent2D target_extent,
	             vk::Format target_format);

	// Records EASU (source -> intermediate) and RCAS (intermediate ->
	// target). The source must already be in ShaderReadOnlyOptimal.
	// The target is left in General layout with shader writes pending;
	// the caller transitions it to its presentation layout.
	void Record(vk::CommandBuffer command, const VulkanImage& source,
	            vk::ImageView source_view, vk::Image target,
	            vk::ImageView target_view);

private:
	bool                        m_enabled         = false;
	vk::Format                  m_format          = vk::Format::eUndefined;
	vk::Sampler                 m_sampler         = nullptr;
	vk::DescriptorSetLayout     m_descriptor_set  = nullptr;
	vk::PipelineLayout          m_easu_layout     = nullptr;
	vk::PipelineLayout          m_rcas_layout     = nullptr;
	vk::Pipeline                m_easu_pipeline   = nullptr;
	vk::Pipeline                m_rcas_pipeline   = nullptr;
	VulkanImage                 m_intermediate;
	vk::ImageView               m_intermediate_view = nullptr;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_FSRUPSCALER_H_
