#include "pch.h"
#include "Renderer.h"
#include "ResourceManager.h"
#include "FlingConfig.h"
#include "File.h"
#include "Image.h"
#include "GraphicsHelpers.h"
#include "Components/Transform.h"
#include <random>

namespace Fling
{
    const int Renderer::MAX_FRAMES_IN_FLIGHT = 2;

    UINT32 Renderer::g_UboIndexPool[MAX_MODEL_MATRIX_BUFFER];
    UINT32 Renderer::g_AllocatedIndex = 0u;

	void Renderer::Init()
	{
		// You must have the registry set before creating a renderer!
		assert(m_Registry);
        InitDevices();

        // Add entt component callbacks for mesh render etc
        InitComponentData();
    }

    void Renderer::InitDevices()
    {
        m_Instance = new Instance();
        assert(m_Instance);

        m_CurrentWindow->CreateSurface(m_Instance->GetRawVkInstance(), &m_Surface);

        m_PhysicalDevice = new PhysicalDevice(m_Instance);
        assert(m_PhysicalDevice);

        m_LogicalDevice = new LogicalDevice(m_Instance, m_PhysicalDevice, m_Surface);
        assert(m_LogicalDevice);

        VkExtent2D Extent = ChooseSwapExtent();
        m_SwapChain = new Swapchain(Extent);
        assert(m_SwapChain);
    }

	void Renderer::InitGraphics()
	{
		CreateRenderPass();
        CreateDescriptorLayout();
        CreateGraphicsPipeline();
        //CreateCommandPool();
        GraphicsHelpers::CreateCommandPool(&m_CommandPool, 0);

        m_DepthBuffer = new DepthBuffer();
        assert(m_DepthBuffer);

        // Create the camera
        float CamMoveSpeed = FlingConfig::GetFloat("Camera", "MoveSpeed", 10.0f);
        float CamRotSpeed = FlingConfig::GetFloat("Camera", "RotationSpeed", 40.0f);
        m_camera = std::make_unique<FirstPersonCamera>(m_CurrentWindow->GetAspectRatio(), CamMoveSpeed, CamRotSpeed);

        CreateFrameBuffers();

        // For testing
        m_TestImage = ResourceManager::LoadResource<Image>("Textures/wood_albedo.png"_hs);

        // Create the dynamic uniform buffers
        PrepareUniformBuffers();

        CreateDescriptorPool();
        CreateDescriptorSets();

        assert(m_Registry);
    
        m_CommandBuffers.resize(m_SwapChainFramebuffers.size());
        GraphicsHelpers::CreateCommandBuffers(
            m_CommandBuffers.data(),
            static_cast<UINT32>(m_CommandBuffers.size()),
            m_CommandPool);

        BuildCommandBuffers(*m_Registry);

        //Intialize imgui
        m_flingImgui = new FlingImgui(m_LogicalDevice, m_SwapChain);
        m_imguiDisplay = ImguiDisplay();
        m_imguiFlag = FlingConfig::GetBool("Imgui", "display");
        InitImgui();

        CreateSyncObjects();
    }

    void Renderer::InitImgui()
    {
        m_flingImgui->Init(
            static_cast<float>(m_CurrentWindow->GetWidth()),
            static_cast<float>(m_CurrentWindow->GetHeight())
        );

        m_flingImgui->InitResources(m_LogicalDevice->GetGraphicsQueue());
        m_flingImgui->SetDisplay<&ImguiDisplay::NewFrame, ImguiDisplay>(m_imguiDisplay);
    }

    void Renderer::UpdateImguiIO()
    {
        //Update imgui mouse events and timings
        ImGuiIO& io = ImGui::GetIO();

        io.DisplaySize = ImVec2(
            static_cast<float>(m_CurrentWindow->GetWidth()),
            static_cast<float>(m_CurrentWindow->GetHeight()));

        io.DeltaTime = Timing::Get().GetDeltaTime();
        io.MousePos = ImVec2(Input::GetMousePos().X, Input::GetMousePos().Y);

        io.MouseDown[0] = Input::IsMouseDown(KeyNames::FL_MOUSE_BUTTON_1);
        io.MouseDown[1] = Input::IsMouseDown(KeyNames::FL_MOUSE_BUTTON_2);
    }

    void Renderer::CreateRenderPass()
    {
        // We have a single color buffer for the images in the swap chain
        VkAttachmentDescription ColorAttachment = {};
        ColorAttachment.format = m_SwapChain->GetImageFormat();
        ColorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        ColorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;       // Clear the frame buffer to black
        ColorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        ColorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        ColorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;

        ColorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        //Change to VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL  for imgui
        ColorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentDescription DepthAttatchment = {};
        DepthAttatchment.format = DepthBuffer::GetDepthBufferFormat();
        DepthAttatchment.samples = VK_SAMPLE_COUNT_1_BIT;
        DepthAttatchment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        DepthAttatchment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        DepthAttatchment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        DepthAttatchment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        DepthAttatchment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        DepthAttatchment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        // Subpass -------------------
        VkAttachmentReference ColorAttachmentRef = {};
        ColorAttachmentRef.attachment = 0;
        ColorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference DepthAttatchmentRef = {};
        DepthAttatchmentRef.attachment = 1;
        DepthAttatchmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription Subpass = {};
        Subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;    // You need to be explicit that this is
                                                                        // a graphics subpass because we may support compute passes in the future
        Subpass.colorAttachmentCount = 1;
        Subpass.pColorAttachments = &ColorAttachmentRef;
        Subpass.pDepthStencilAttachment = &DepthAttatchmentRef;

        // Add a subpass dependency
        VkSubpassDependency dependency = {};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        // Create the render pass
        std::array<VkAttachmentDescription, 2> Attachments = { ColorAttachment, DepthAttatchment };
        VkRenderPassCreateInfo renderPassInfo = {};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = static_cast<UINT32>(Attachments.size());
        renderPassInfo.pAttachments = Attachments.data();
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &Subpass;

        renderPassInfo.dependencyCount = 1;
        renderPassInfo.pDependencies = &dependency;

        if (vkCreateRenderPass(m_LogicalDevice->GetVkDevice(), &renderPassInfo, nullptr, &m_RenderPass) != VK_SUCCESS)
        {
            F_LOG_FATAL("Failed to create render pass!");
        }
    }

    void Renderer::CreateDescriptorLayout()
    {
        std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings =
        {
            Initalizers::DescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT, 0),
            Initalizers::DescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_SHADER_STAGE_VERTEX_BIT, 1),
            Initalizers::DescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 2)
        };

        // TODO Create descript set bindings based on vertex shader resource layout
        if (m_ShaderProgram->HasStage(ShaderStage::Vertex))
        {
            const ResourceLayout& Resoure = m_ShaderProgram->GetShader(ShaderStage::Vertex)->GetResourceLayout();
            for (size_t i = 0; i < VULKAN_NUM_DESCRIPTOR_SETS; ++i)
            {                
                
            }
        }

        VkDescriptorSetLayoutCreateInfo LayoutInfo = {};
        LayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        LayoutInfo.bindingCount = static_cast<UINT32>(setLayoutBindings.size());
        LayoutInfo.pBindings = setLayoutBindings.data();

        if (vkCreateDescriptorSetLayout(m_LogicalDevice->GetVkDevice(), &LayoutInfo, nullptr, &m_DescriptorSetLayout) != VK_SUCCESS)
        {
            F_LOG_FATAL("Failed to create descipror set layout!");
        }
    }

    void Renderer::CreateGraphicsPipeline()
    {
        if (!m_ShaderProgram)
        {
            F_LOG_FATAL("You must specify the shader program for the Fling Renderer to use!");
            return;
        }

        m_ShaderProgram->LoadShaders();

        // Shader stage creation!
        VkPipelineShaderStageCreateInfo ShaderStages[static_cast<unsigned>(ShaderStage::Count)];
        unsigned num_stages = 0;

        VkSpecializationInfo spec_info[static_cast<unsigned>(ShaderStage::Count)] = {};

        // This is bad because then I can't have multiple vert/frag shaders
        for (unsigned i = 0; i < static_cast<unsigned>(ShaderStage::Count); i++)
        {
            ShaderStage stage = static_cast<ShaderStage>(i);
            // If we have this shader stage in the graphic program
            const Guid& ShaderName = m_ShaderProgram->GetStage(stage);
            if (ShaderName != INVALID_GUID)
            {
                if (const std::shared_ptr<Fling::Shader>& Shader = Shader::Create(ShaderName, stage))
                {
                    VkPipelineShaderStageCreateInfo& createInfo = ShaderStages[num_stages++];
                    createInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                    createInfo.module = Shader->GetShaderModule();
                    createInfo.stage = Shader->GetVkBindStage();
                    //createInfo.stage = static_cast<VkShaderStageFlagBits>(1u << i);
                    createInfo.pName = "main";
                    createInfo.flags = 0;
                    createInfo.pNext = nullptr;
                    createInfo.pSpecializationInfo = nullptr;
                }
            }   
        }

        // Vertex input ----------------------
        VkVertexInputBindingDescription BindingDescription = Vertex::GetBindingDescription();
        std::array<VkVertexInputAttributeDescription, 3> AttributeDescriptions = Vertex::GetAttributeDescriptions();

        VkPipelineVertexInputStateCreateInfo VertexInputInfo = {};
        VertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VertexInputInfo.vertexBindingDescriptionCount = 1;
        VertexInputInfo.pVertexBindingDescriptions = &BindingDescription;
        VertexInputInfo.vertexAttributeDescriptionCount = static_cast<UINT32>(AttributeDescriptions.size());;
        VertexInputInfo.pVertexAttributeDescriptions = AttributeDescriptions.data();

        // Input Assembly ----------------------
        VkPipelineInputAssemblyStateCreateInfo InputAssembly = {};
        InputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        InputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        InputAssembly.primitiveRestartEnable = VK_FALSE;

        // View ports and scissors ----------------------
        VkViewport viewport = {};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = (float)m_SwapChain->GetExtents().width;       // These values can differ from the width/height of the window!
        viewport.height = (float)m_SwapChain->GetExtents().height;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;

        VkRect2D scissor = {};
        scissor.offset = { 0, 0 };
        scissor.extent = m_SwapChain->GetExtents();

        VkPipelineViewportStateCreateInfo viewportState = {};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.pViewports = &viewport;
        viewportState.scissorCount = 1;
        viewportState.pScissors = &scissor;

        // Rasterizer ----------------------------------
        VkPipelineRasterizationStateCreateInfo Rasterizer = {};
        {
            Rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            Rasterizer.depthClampEnable = VK_FALSE;
            Rasterizer.rasterizerDiscardEnable = VK_FALSE;  // Useful for shadow maps, using would require enabling a GPU feature
            Rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
            Rasterizer.lineWidth = 1.0f;

            Rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
            Rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; // Specify the vertex order! 

            Rasterizer.depthBiasEnable = VK_FALSE;
            Rasterizer.depthBiasConstantFactor = 0.0f;  // Optional
            Rasterizer.depthBiasClamp = 0.0f;           // Optional
            Rasterizer.depthBiasSlopeFactor = 0.0f;     // Optional
        }

        // Multi-sampling ----------------------------------
        // Can be a cheaper way to perform anti-aliasing
        // Using it requires enabling a GPU feature
        VkPipelineMultisampleStateCreateInfo Multisampling = {};
        {
            Multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            Multisampling.sampleShadingEnable = VK_FALSE;
            Multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            Multisampling.minSampleShading = 1.0f; // Optional
            Multisampling.pSampleMask = nullptr; // Optional
            Multisampling.alphaToCoverageEnable = VK_FALSE; // Optional
            Multisampling.alphaToOneEnable = VK_FALSE; // Optional
        }

        // Color blending ----------------------------------
        VkPipelineColorBlendAttachmentState ColorBlendAttachment = {};
        VkPipelineColorBlendStateCreateInfo ColorBlending = {};
        {
            // Attatchment
            ColorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            ColorBlendAttachment.blendEnable = VK_FALSE;
            ColorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;     // Optional
            ColorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;    // Optional
            ColorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;                // Optional
            ColorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;     // Optional
            ColorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;    // Optional
            ColorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;                // Optional

            ColorBlendAttachment.blendEnable = VK_TRUE;
            ColorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            ColorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            ColorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
            ColorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            ColorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            ColorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

            // Blend
            ColorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            ColorBlending.logicOpEnable = VK_FALSE;
            ColorBlending.logicOp = VK_LOGIC_OP_COPY;   // Optional
            ColorBlending.attachmentCount = 1;
            ColorBlending.pAttachments = &ColorBlendAttachment;
            ColorBlending.blendConstants[0] = 0.0f;     // Optional
            ColorBlending.blendConstants[1] = 0.0f;     // Optional
            ColorBlending.blendConstants[2] = 0.0f;     // Optional
            ColorBlending.blendConstants[3] = 0.0f;     // Optional
        }

        // Pipeline layout ---------------------
        VkPipelineLayoutCreateInfo PipelineLayoutInfo = {};
        PipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        PipelineLayoutInfo.setLayoutCount = 1;
        PipelineLayoutInfo.pSetLayouts = &m_DescriptorSetLayout;
        PipelineLayoutInfo.pushConstantRangeCount = 0;      // Optional
        PipelineLayoutInfo.pPushConstantRanges = nullptr;   // Optional

        if (vkCreatePipelineLayout(m_LogicalDevice->GetVkDevice(), &PipelineLayoutInfo, nullptr, &m_PipelineLayout) != VK_SUCCESS)
        {
            F_LOG_FATAL("Failed to create pipeline layout!");
        }

		// Depth Stencil
        VkPipelineDepthStencilStateCreateInfo depthStencil = {};
        {
            depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            depthStencil.depthTestEnable = VK_TRUE;
            depthStencil.depthWriteEnable = VK_TRUE;
            depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
            depthStencil.depthBoundsTestEnable = VK_FALSE;
            depthStencil.minDepthBounds = 0.0f;
            depthStencil.maxDepthBounds = 1.0f;
            depthStencil.stencilTestEnable = VK_FALSE;
            depthStencil.front = {};
            depthStencil.back = {};
        }

        // Create graphics pipeline ------------------------
        VkGraphicsPipelineCreateInfo pipelineInfo = {};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = num_stages;
        pipelineInfo.pStages = ShaderStages;

        pipelineInfo.pVertexInputState = &VertexInputInfo;
        pipelineInfo.pInputAssemblyState = &InputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &Rasterizer;
        pipelineInfo.pMultisampleState = &Multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &ColorBlending;
        pipelineInfo.pDynamicState = nullptr; // Optional

        pipelineInfo.layout = m_PipelineLayout;
        pipelineInfo.renderPass = m_RenderPass;
        pipelineInfo.subpass = 0;

        pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;   // Optional
        pipelineInfo.basePipelineIndex = -1;                // Optional

        if (vkCreateGraphicsPipelines(m_LogicalDevice->GetVkDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_GraphicsPipeline) != VK_SUCCESS)
        {
            F_LOG_FATAL("failed to create graphics pipeline!");
        }
    }

    void Renderer::CreateFrameBuffers()
    {
        assert(m_SwapChain && m_DepthBuffer);

        m_SwapChainFramebuffers.resize(m_SwapChain->GetImageViewCount());

        const std::vector<VkImageView>& ImageViews = m_SwapChain->GetImageViews();
        
        // Create the frame buffers based on the image views
        for (size_t i = 0; i < m_SwapChain->GetImageViewCount(); i++)
        {
            std::array<VkImageView, 2> attachments =
            {
                ImageViews[i],
                m_DepthBuffer->GetVkImageView()
            };

            VkFramebufferCreateInfo framebufferInfo = {};
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = m_RenderPass;
            framebufferInfo.attachmentCount = static_cast<UINT32>(attachments.size());
            framebufferInfo.pAttachments = attachments.data();
            framebufferInfo.width = m_SwapChain->GetExtents().width;
            framebufferInfo.height = m_SwapChain->GetExtents().height;
            framebufferInfo.layers = 1;

            if (vkCreateFramebuffer(m_LogicalDevice->GetVkDevice(), &framebufferInfo, nullptr, &m_SwapChainFramebuffers[i]) != VK_SUCCESS)
            {
                F_LOG_FATAL("Failed to create framebuffer!");
            }
        }
    }

    void Renderer::BuildCommandBuffers(entt::registry& t_Reg)
    {
        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
        beginInfo.pInheritanceInfo = nullptr;

        // Start command buffer recording
        for (size_t i = 0; i < m_CommandBuffers.size(); i++)
        {
            if (vkBeginCommandBuffer(m_CommandBuffers[i], &beginInfo) != VK_SUCCESS)
            {
                F_LOG_FATAL("Failed to begin recording command buffer!");
            }

            VkRenderPassBeginInfo renderPassInfo = {};
            renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            renderPassInfo.renderPass = m_RenderPass;
            renderPassInfo.framebuffer = m_SwapChainFramebuffers[i];

            renderPassInfo.renderArea.offset = { 0, 0 };
            renderPassInfo.renderArea.extent = m_SwapChain->GetExtents();

            // Clear values ---------------------
            std::array<VkClearValue, 2> clearValues = {};
            clearValues[0].color = { 0.0f, 0.0f, 0.0f, 1.0f };
            clearValues[1].depthStencil = { 1.0f, 0 };
            renderPassInfo.clearValueCount = static_cast<UINT32>(clearValues.size());
            renderPassInfo.pClearValues = clearValues.data();

            vkCmdBeginRenderPass(m_CommandBuffers[i], &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

            vkCmdBindPipeline(m_CommandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, m_GraphicsPipeline);

            // For each active mesh, get it's index
            t_Reg.view<MeshRenderer, Transform>().each([&](MeshRenderer& t_MeshRend, Transform& t_Trans)
            {
                Fling::Model* Model = t_MeshRend.m_Model;
                if (Model)
                {
                    VkBuffer vertexBuffers[1] = { Model->GetVertexBuffer()->GetVkBuffer() };
                    VkDeviceSize offsets[1] = { 0 };
                    vkCmdBindVertexBuffers(m_CommandBuffers[i], 0, 1, vertexBuffers, offsets);
                    vkCmdBindIndexBuffer(m_CommandBuffers[i], Model->GetIndexBuffer()->GetVkBuffer(), 0, Model->GetIndexType());
                    // Bind the descriptor set for rendering a mesh using the dynamic offset
                    vkCmdBindDescriptorSets(m_CommandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, m_PipelineLayout, 0, 1, &m_DescriptorSets[i], 1, &t_MeshRend.m_ModelMatrixOffset);

                    vkCmdDrawIndexed(m_CommandBuffers[i], Model->GetIndexCount(), 1, 0, 0, 0);
                }
                else
                {
                    F_LOG_WARN("Model is invalid on mesh renderer!");
                }

            });

            vkCmdEndRenderPass(m_CommandBuffers[i]);

            if (vkEndCommandBuffer(m_CommandBuffers[i]) != VK_SUCCESS)
            {
                F_LOG_FATAL("failed to record command buffer!");
            }
        }
    }

    void Renderer::CreateSyncObjects()
    {
        m_ImageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
        m_RenderFinishedSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
        m_InFlightFences.resize(MAX_FRAMES_IN_FLIGHT);

        VkSemaphoreCreateInfo semaphoreInfo = {};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        VkFenceCreateInfo fenceInfo = {};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            if (vkCreateSemaphore(m_LogicalDevice->GetVkDevice(), &semaphoreInfo, nullptr, &m_ImageAvailableSemaphores[i]) != VK_SUCCESS ||
                vkCreateSemaphore(m_LogicalDevice->GetVkDevice(), &semaphoreInfo, nullptr, &m_RenderFinishedSemaphores[i]) != VK_SUCCESS ||
                vkCreateFence(m_LogicalDevice->GetVkDevice(), &fenceInfo, nullptr, &m_InFlightFences[i]) != VK_SUCCESS)
            {
                F_LOG_FATAL("Failed to create semaphores or fence!");
            }
        }
    }

    void Renderer::CleanupFrameResources()
    {
        m_DepthBuffer->Cleanup();

        for (size_t i = 0; i < m_SwapChainFramebuffers.size(); i++)
        {
            vkDestroyFramebuffer(m_LogicalDevice->GetVkDevice(), m_SwapChainFramebuffers[i], nullptr);
        }

        vkFreeCommandBuffers(m_LogicalDevice->GetVkDevice(), m_CommandPool, static_cast<UINT32>(m_CommandBuffers.size()), m_CommandBuffers.data());

        vkDestroyPipeline(m_LogicalDevice->GetVkDevice(), m_GraphicsPipeline, nullptr);
        vkDestroyPipelineLayout(m_LogicalDevice->GetVkDevice(), m_PipelineLayout, nullptr);
        vkDestroyRenderPass(m_LogicalDevice->GetVkDevice(), m_RenderPass, nullptr);

        m_SwapChain->Cleanup();

        // Cleanup uniform buffers -------------------------
        for (size_t i = 0; i < m_DynamicUniformBuffers.size(); ++i)
        {
            m_DynamicUniformBuffers[i].Release();
        }

        vkDestroyDescriptorPool(m_LogicalDevice->GetVkDevice(), m_DescriptorPool, nullptr);
    }

    void Renderer::RecreateFrameResources()
    {
        m_CurrentWindow->RecreateSwapChain();
        m_LogicalDevice->WaitForIdle();

        CleanupFrameResources();
        m_flingImgui->Release();

        m_SwapChain->Recreate(ChooseSwapExtent());

        CreateRenderPass();
        CreateGraphicsPipeline();

        m_DepthBuffer->Create();

        CreateFrameBuffers();

        PrepareUniformBuffers();
        CreateDescriptorPool();
        CreateDescriptorSets();

        assert(m_Registry);

        m_CommandBuffers.resize(m_SwapChainFramebuffers.size());
        GraphicsHelpers::CreateCommandBuffers(
            m_CommandBuffers.data(), 
            static_cast<UINT32>(m_CommandBuffers.size()), 
            m_CommandPool);

        BuildCommandBuffers(*m_Registry);
        InitImgui();
    }

    void Renderer::PrepareUniformBuffers()
    {
        // Resize the number of dynamic UBO objects
        size_t ImageCount = m_SwapChain->GetImages().size();
        m_DynamicUniformBuffers.resize(ImageCount);

        // Calculate required alignment based on minimum device offset alignment
        size_t MinUboAlignment = m_PhysicalDevice->GetDeviceProps().limits.minUniformBufferOffsetAlignment;
        m_DynamicAlignment = sizeof(glm::mat4);
        if (MinUboAlignment > 0)
        {
            m_DynamicAlignment = (m_DynamicAlignment + MinUboAlignment - 1) & ~(MinUboAlignment - 1);
        }

        F_LOG_TRACE("MinUboAlignment: {}", MinUboAlignment);
        F_LOG_TRACE("Dynamic Alignment: {}", m_DynamicAlignment);

        // Initialize the dynamic UBO's
        for (size_t i = 0; i < ImageCount; ++i)
        {
            size_t bufferSize = MAX_MODEL_MATRIX_BUFFER * m_DynamicAlignment;
            m_DynamicUniformBuffers[i].Model = (glm::mat4*)Fling::AlignedAlloc(bufferSize, m_DynamicAlignment);
            // Initalize the model matrix to the identity
            assert(m_DynamicUniformBuffers[i].Model);
            *m_DynamicUniformBuffers[i].Model = glm::identity<glm::mat4>();

            // Create the view uniform
            m_DynamicUniformBuffers[i].View = new Buffer(
                /* t_Size */ sizeof(m_UboVS),
                /* t_Usage */ VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                /* t_Properties */ VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
            );

            // Note that we do not specify the VK_MEMORY_PROPERTY_HOST_COHERENT_BIT so we have to flush 
            // the dynamic buffer manually. This is so that we only have to 
            m_DynamicUniformBuffers[i].Dynamic = new Buffer(
                /* t_Size */ bufferSize,
                /* t_Usage */ VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                /* t_Properties */ VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
            );

            VkResult ViewRes = m_DynamicUniformBuffers[i].View->MapMemory();
            assert(ViewRes == VK_SUCCESS);
            VkResult DynamicRes = m_DynamicUniformBuffers[i].Dynamic->MapMemory();
            assert(DynamicRes == VK_SUCCESS);
        }

        // Prep the pool of indecies
        for (size_t i = 0; i < MAX_MODEL_MATRIX_BUFFER; ++i)
        {
            g_UboIndexPool[i] = i * m_DynamicAlignment;
        }

        UpdateUniformBuffer(m_SwapChain->GetActiveImageIndex());
        UpdateDynamicUniformBuffer(m_SwapChain->GetActiveImageIndex());
    }

    void Renderer::CreateDescriptorPool()
    {
        UINT32 SwapImageCount = static_cast<UINT32>(m_SwapChain->GetImageCount());

        std::vector<VkDescriptorPoolSize> PoolSizes =
        {
            Initalizers::DescriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, SwapImageCount),
            Initalizers::DescriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, SwapImageCount),
            Initalizers::DescriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SwapImageCount),
        };

        VkDescriptorPoolCreateInfo PoolInfo = {};
        PoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        PoolInfo.poolSizeCount = static_cast<UINT32>(PoolSizes.size());
        PoolInfo.pPoolSizes = PoolSizes.data();
        PoolInfo.maxSets = SwapImageCount;

        if (vkCreateDescriptorPool(m_LogicalDevice->GetVkDevice(), &PoolInfo, nullptr, &m_DescriptorPool) != VK_SUCCESS)
        {
            F_LOG_FATAL("Failed to create discriptor pool!");
        }

        // Get the descriptor layout info from shader reflection data!
        // Again, grabbed form Granite
        CombinedResourceLayout layout;
        if (m_ShaderProgram->HasStage(ShaderStage::Vertex))
        {
            layout.attribute_mask = m_ShaderProgram->GetShader(ShaderStage::Vertex)->GetResourceLayout().input_mask;
        }
        if (m_ShaderProgram->HasStage(ShaderStage::Fragment))
        {
            layout.render_target_mask = m_ShaderProgram->GetShader(ShaderStage::Fragment)->GetResourceLayout().output_mask;
        }


        layout.descriptor_set_mask = 0;
        for (unsigned i = 0; i < static_cast<unsigned>(ShaderStage::Count); i++)
        {
            std::shared_ptr<Shader> shader = m_ShaderProgram->GetShader(static_cast<ShaderStage>(i));
            if (!shader)
            {
                continue;
            }

            uint32_t stage_mask = 1u << i;

            const ResourceLayout& shader_layout = shader->GetResourceLayout();
            for (unsigned set = 0; set < VULKAN_NUM_DESCRIPTOR_SETS; set++)
            {
                layout.sets[set].sampled_image_mask |= shader_layout.sets[set].sampled_image_mask;
                layout.sets[set].storage_image_mask |= shader_layout.sets[set].storage_image_mask;
                layout.sets[set].uniform_buffer_mask |= shader_layout.sets[set].uniform_buffer_mask;
                layout.sets[set].storage_buffer_mask |= shader_layout.sets[set].storage_buffer_mask;
                layout.sets[set].sampled_buffer_mask |= shader_layout.sets[set].sampled_buffer_mask;
                layout.sets[set].input_attachment_mask |= shader_layout.sets[set].input_attachment_mask;
                layout.sets[set].sampler_mask |= shader_layout.sets[set].sampler_mask;
                layout.sets[set].separate_image_mask |= shader_layout.sets[set].separate_image_mask;
                layout.sets[set].fp_mask |= shader_layout.sets[set].fp_mask;

                for_each_bit(shader_layout.sets[set].immutable_sampler_mask, [&](UINT32 binding) {
                    StockSampler sampler = get_immutable_sampler(shader_layout.sets[set], binding);

                    // Do we already have an immutable sampler? Make sure it matches the layout.
                    if (has_immutable_sampler(layout.sets[set], binding))
                    {
                        if (sampler != get_immutable_sampler(layout.sets[set], binding))
                        {
                            F_LOG_ERROR("Immutable sampler mismatch detected!\n");
                        }
                    }

                    set_immutable_sampler(layout.sets[set], binding, sampler);
                });

                uint32_t active_binds =
                    shader_layout.sets[set].sampled_image_mask |
                    shader_layout.sets[set].storage_image_mask |
                    shader_layout.sets[set].uniform_buffer_mask |
                    shader_layout.sets[set].storage_buffer_mask |
                    shader_layout.sets[set].sampled_buffer_mask |
                    shader_layout.sets[set].input_attachment_mask |
                    shader_layout.sets[set].sampler_mask |
                    shader_layout.sets[set].separate_image_mask;

                if (active_binds)
                {
                    layout.stages_for_sets[set] |= stage_mask;
                }

                for_each_bit(active_binds, [&](UINT32 bit) {
                    layout.stages_for_bindings[set][bit] |= stage_mask;

                    auto& combined_size = layout.sets[set].array_size[bit];
                    auto& shader_size = shader_layout.sets[set].array_size[bit];
                    if (combined_size && combined_size != shader_size)
                    {
                        F_LOG_ERROR("Mismatch between array sizes in different shaders.\n");
                    }
                    else
                    {
                        combined_size = shader_size;
                    }
                });

            }

            // Merge push constant ranges into one range.
            // Do not try to split into multiple ranges as it just complicates things for no obvious gain.
            if (shader_layout.push_constant_size != 0)
            {
                layout.push_constant_range.stageFlags |= 1u << i;
                layout.push_constant_range.size = std::max(layout.push_constant_range.size, shader_layout.push_constant_size);
            }

            layout.spec_constant_mask[i] = shader_layout.spec_constant_mask;
            layout.combined_spec_constant_mask |= shader_layout.spec_constant_mask;

            for (unsigned set = 0; set < VULKAN_NUM_DESCRIPTOR_SETS; set++)
            {
                if (layout.stages_for_sets[set] != 0)
                {
                    layout.descriptor_set_mask |= 1u << set;

                    for (unsigned binding = 0; binding < VULKAN_NUM_BINDINGS; binding++)
                    {
                        auto& array_size = layout.sets[set].array_size[binding];
                        if (array_size == 0)
                        {
                            array_size = 1;
                        }
                        else
                        {
                            for (unsigned i = 1; i < array_size; i++)
                            {
                                if (layout.stages_for_bindings[set][binding + i] != 0)
                                {
                                    F_LOG_ERROR("Detected binding aliasing for (%u, %u). Binding array with %u elements starting at (%u, %u) overlaps.\n",
                                        set, binding + i, array_size, set, binding);
                                }
                            }
                        }
                    }
                }
            }
        }

        // Request a pipeline layout with this combined resource! 

    }

    void Renderer::CreateDescriptorSets()
    {
        const std::vector<VkImage>& Images = m_SwapChain->GetImages();

        // Specify what descriptor pool to allocate from and how many
        std::vector<VkDescriptorSetLayout> layouts(Images.size(), m_DescriptorSetLayout);
        VkDescriptorSetAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_DescriptorPool;
        allocInfo.descriptorSetCount = static_cast<UINT32>(Images.size());
        allocInfo.pSetLayouts = layouts.data();

        m_DescriptorSets.resize(Images.size());
        // Sets will be cleaned up when the descriptor pool is, no need for an explicit free call in cleanup
        if (vkAllocateDescriptorSets(m_LogicalDevice->GetVkDevice(), &allocInfo, m_DescriptorSets.data()) != VK_SUCCESS)
        {
            F_LOG_FATAL("Failed to allocate descriptor sets!");
        }

        // Configure descriptor sets
        for (size_t i = 0; i < Images.size(); ++i)
        {
            std::array<VkWriteDescriptorSet, 3> descriptorWrites = {};
            // Binding 0 : Projection/view matrix uniform buffer
            VkDescriptorBufferInfo BufferInfo = {};
            BufferInfo.buffer = m_DynamicUniformBuffers[i].View->GetVkBuffer();
            BufferInfo.offset = 0;
            BufferInfo.range = VK_WHOLE_SIZE;
            descriptorWrites[0] = Initalizers::WriteDescriptorSet(
                m_DescriptorSets[i],
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                0,
                &BufferInfo
            );

            // Binding 1 : Instance matrix as dynamic uniform buffer
            VkDescriptorBufferInfo DynamicBufferInfo = {};
            DynamicBufferInfo.buffer = m_DynamicUniformBuffers[i].Dynamic->GetVkBuffer();
            DynamicBufferInfo.offset = 0;
            DynamicBufferInfo.range = VK_WHOLE_SIZE;
            descriptorWrites[1] = Initalizers::WriteDescriptorSet(
                m_DescriptorSets[i],
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                1,
                &DynamicBufferInfo
            );

            // Binding 2 : Image sampler
            VkDescriptorImageInfo imageInfo = {};
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfo.imageView = m_TestImage->GetVkImageView();
            imageInfo.sampler = m_TestImage->GetSampler();

            descriptorWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[2].dstSet = m_DescriptorSets[i];
            descriptorWrites[2].dstBinding = 2;
            descriptorWrites[2].dstArrayElement = 0;
            descriptorWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            descriptorWrites[2].descriptorCount = 1;
            descriptorWrites[2].pImageInfo = &imageInfo;

            vkUpdateDescriptorSets(m_LogicalDevice->GetVkDevice(), static_cast<UINT32>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);
        }
    }

    // Swapchain support --------------------------------------

    VkExtent2D Renderer::ChooseSwapExtent()
    {
        VkSurfaceCapabilitiesKHR t_Capabilies = {};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_PhysicalDevice->GetVkPhysicalDevice(), m_Surface, &t_Capabilies);

        if (t_Capabilies.currentExtent.width != std::numeric_limits<UINT32>::max())
        {
            return t_Capabilies.currentExtent;
        }
        else
        {
            UINT32 width = m_CurrentWindow->GetWidth();
            UINT32 height = m_CurrentWindow->GetHeight();

            VkExtent2D actualExtent = { width, height };

            actualExtent.width = std::max(t_Capabilies.minImageExtent.width,
                std::min(t_Capabilies.maxImageExtent.width, actualExtent.width));
            actualExtent.height = std::max(t_Capabilies.minImageExtent.height,
                std::min(t_Capabilies.maxImageExtent.height, actualExtent.height));

            return actualExtent;
        }
    }

    void Renderer::CreateGameWindow(const UINT32 t_width, const UINT32 t_height)
    {
        WindowProps Props = {};
        Props.m_Height = t_width;
        Props.m_Width = t_width;

        // Ensure the window width is valid
        if (t_width > 0 && t_width < 5000 && t_height > 0 && t_height < 5000)
        {
            Props.m_Height = t_width;
            Props.m_Height = t_height;
        }
        else
        {
            F_LOG_ERROR("Window Width of {} or height of {} is invalid! Using default values", t_width, t_height);
            Props.m_Height = FLING_DEFAULT_WINDOW_WIDTH;
            Props.m_Height = FLING_DEFAULT_WINDOW_HEIGHT;
        }

        // Get the window title
        std::string Title = FlingConfig::GetString("Engine", "WindowTitle");

        if (FlingConfig::GetBool("Engine", "DisplayVersionInfoInTitle", true))
        {
            Title += " // Version: " + Version::EngineVersion.ToString();
        }

        if (FlingConfig::GetBool("Engine", "DisplayBuildInfoInTitle", true))
        {
            Title += " // Built from " + (std::string)(GIT_BRANCH) + " @ " + GIT_COMMIT_HASH;
        }

        Props.m_Title = Title;

        m_CurrentWindow = FlingWindow::Create(Props);
    }

    void Renderer::Tick(float DeltaTime)
    {
        m_CurrentWindow->Update();

        m_camera->Update(DeltaTime);

        //toggle imgui
        if (Input::IsKeyDown(KeyNames::FL_KEY_I))
        {
            m_imguiFlag = !m_imguiFlag;
        }
    }

    void Renderer::DrawFrame(entt::registry& t_Reg)
    {
        UpdateImguiIO();

        VkResult iResult = m_SwapChain->AquireNextImage(m_ImageAvailableSemaphores[CurrentFrameIndex]);
        UINT32  ImageIndex = m_SwapChain->GetActiveImageIndex();

        vkResetFences(m_LogicalDevice->GetVkDevice(), 1, &m_InFlightFences[CurrentFrameIndex]);

        //Update imgui command buffers
        {
            vkResetCommandPool(m_LogicalDevice->GetVkDevice(), m_flingImgui->GetCommandPool(), 0);
            m_flingImgui->BuildCommandBuffers(m_imguiFlag);
        }
        
        // Check if the swap chain needs to be recreated
        if (iResult == VK_ERROR_OUT_OF_DATE_KHR)
        {
            RecreateFrameResources();
            return;
        }
        else if (iResult != VK_SUCCESS && iResult != VK_SUBOPTIMAL_KHR)
        {
            F_LOG_FATAL("Failed to acquire swap chain image!");
        }

        UpdateUniformBuffer(ImageIndex);
        UpdateDynamicUniformBuffer(ImageIndex);

        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

        VkSemaphore waitSemaphores[] = { m_ImageAvailableSemaphores[CurrentFrameIndex] };
        VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;

        std::array<VkCommandBuffer, 2> submitCommandBuffers =
        {
            m_CommandBuffers[ImageIndex], m_flingImgui->GetCommandBuffer(ImageIndex)
        };


        submitInfo.commandBufferCount = static_cast<UINT32>(submitCommandBuffers.size());
        submitInfo.pCommandBuffers = submitCommandBuffers.data();

        VkSemaphore signalSemaphores[] = { m_RenderFinishedSemaphores[CurrentFrameIndex] };
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;

        if (vkQueueSubmit(m_LogicalDevice->GetGraphicsQueue(), 1, &submitInfo, m_InFlightFences[CurrentFrameIndex]) != VK_SUCCESS)
        {
            F_LOG_FATAL("Failed to submit draw command buffer!");
        }

        vkWaitForFences(m_LogicalDevice->GetVkDevice(), 1, &m_InFlightFences[CurrentFrameIndex], VK_TRUE, std::numeric_limits<uint64_t>::max());

        iResult = m_SwapChain->QueuePresent(m_LogicalDevice->GetPresentQueue(), *signalSemaphores);

        if (iResult == VK_ERROR_OUT_OF_DATE_KHR || iResult == VK_SUBOPTIMAL_KHR || m_FrameBufferResized)
        {
            m_FrameBufferResized = false;
            RecreateFrameResources();
        }
        else if (iResult != VK_SUCCESS)
        {
            F_LOG_FATAL("Failed to present swap chain image!");
        }


        CurrentFrameIndex = (CurrentFrameIndex + 1) % MAX_FRAMES_IN_FLIGHT;
    }

    void Renderer::UpdateUniformBuffer(UINT32 t_CurrentImage)
    {
        m_UboVS.View = m_camera->GetViewMatrix();
        m_UboVS.Projection = m_camera->GetProjectionMatrix();

        // The Y coordinate needs to be invertex in vulkan because open GL points up
        m_UboVS.Projection[1][1] *= -1.0f;

        // Copy the non-dynamic ubo data to the GPU
        memcpy(m_DynamicUniformBuffers[t_CurrentImage].View->m_MappedMem, &m_UboVS, sizeof(m_UboVS));
    }

    void Renderer::UpdateDynamicUniformBuffer(UINT32 t_CurrentImage)
    {
        // For each active mesh renderer
        m_Registry->view<MeshRenderer, Transform>().each([&](MeshRenderer& t_MeshRend, Transform& t_Trans)
        {
            // Calculate the world matrix based on the given transform
            glm::mat4* modelMat = (glm::mat4*)(((uint64_t)m_DynamicUniformBuffers[t_CurrentImage].Model + (t_MeshRend.m_ModelMatrixOffset)));
            Transform::CalculateWorldMatrix(t_Trans, modelMat);
        });

        // Copy the CPU model matrices to the GPU (dynamic mapped UBO mem)
        memcpy(
            m_DynamicUniformBuffers[t_CurrentImage].Dynamic->m_MappedMem,
            m_DynamicUniformBuffers[t_CurrentImage].Model,
            m_DynamicUniformBuffers[t_CurrentImage].Dynamic->GetSize()
        );

        // Manually flush to only update what has changed
        VkMappedMemoryRange memoryRange = Initalizers::MappedMemoryRange();
        memoryRange.memory = m_DynamicUniformBuffers[t_CurrentImage].Dynamic->GetVkDeviceMemory();
        memoryRange.size = m_DynamicUniformBuffers[t_CurrentImage].Dynamic->GetSize();
        vkFlushMappedMemoryRanges(m_LogicalDevice->GetVkDevice(), 1, &memoryRange);
    }

    // Shutdown steps -------------------------------------------

    void Renderer::PrepShutdown()
    {
        m_LogicalDevice->WaitForIdle();
        // You have to free images before before the renderer gets shutdown because they need to the 
        // physical device free their VK resources
        if (m_TestImage)
        {
            m_TestImage.reset();
        }
    }

    void Renderer::Shutdown()
    {
        // Cleanup Vulkan ------
        CleanupFrameResources();

        if (m_flingImgui)
        {
            delete m_flingImgui;
            m_flingImgui = nullptr;
        }

        if (m_SwapChain)
        {
            delete m_SwapChain;
            m_SwapChain = nullptr;
        }

        vkDestroyDescriptorSetLayout(m_LogicalDevice->GetVkDevice(), m_DescriptorSetLayout, nullptr);

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            vkDestroySemaphore(m_LogicalDevice->GetVkDevice(), m_RenderFinishedSemaphores[i], nullptr);
            vkDestroySemaphore(m_LogicalDevice->GetVkDevice(), m_ImageAvailableSemaphores[i], nullptr);
            vkDestroyFence(m_LogicalDevice->GetVkDevice(), m_InFlightFences[i], nullptr);
        }

        vkDestroyCommandPool(m_LogicalDevice->GetVkDevice(), m_CommandPool, nullptr);

        if (m_LogicalDevice)
        {
            delete m_LogicalDevice;
            m_LogicalDevice = nullptr;
        }

        vkDestroySurfaceKHR(m_Instance->GetRawVkInstance(), m_Surface, nullptr);

        if (m_PhysicalDevice)
        {
            delete m_PhysicalDevice;
            m_PhysicalDevice = nullptr;
        }

        if (m_Instance)
        {
            delete m_Instance;
            m_Instance = nullptr;
        }

        // Cleanup Window --------
        if (m_CurrentWindow)
        {
            delete m_CurrentWindow;
            m_CurrentWindow = nullptr;
        }
    }

    // @see https://github.com/skypjack/entt/wiki/Crash-Course:-entity-component-system#observe-changes
    void Renderer::InitComponentData()
    {
        // Add any component callbacks that we may need
        m_Registry->on_construct<MeshRenderer>().connect<&Renderer::MeshRendererAdded>(*this);
    }

    void Renderer::MeshRendererAdded(entt::entity t_Ent, entt::registry& t_Reg, MeshRenderer& t_MeshRend)
    {
        std::shared_ptr<Model> Model = Model::Create(entt::hashed_string{ t_MeshRend.MeshName.c_str() });

        assert(Model);

        t_MeshRend.Initalize(Model.get(), GetAvailableModelMatrix());
        SetFrameBufferHasBeenResized(true);
    }

    UINT32 Renderer::GetAvailableModelMatrix()
    {
        const uint32_t index = g_AllocatedIndex++;
        // Multiply by dynamic alignment
        return (g_UboIndexPool[index & (MAX_MODEL_MATRIX_BUFFER - 1u)]);
    }
}    // namespace Fling