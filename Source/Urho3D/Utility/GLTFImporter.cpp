//
// Copyright (c) 2017-2020 the rbfx project.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "../Precompiled.h"

#include "../Core/Context.h"
#include "../Core/Exception.h"
#include "../Graphics/AnimatedModel.h"
#include "../Graphics/Light.h"
#include "../Graphics/Material.h"
#include "../Graphics/Model.h"
#include "../Graphics/ModelView.h"
#include "../Graphics/Octree.h"
#include "../Graphics/Technique.h"
#include "../Graphics/Texture.h"
#include "../Graphics/Texture2D.h"
#include "../Graphics/TextureCube.h"
#include "../Graphics/Skybox.h"
#include "../Graphics/StaticModel.h"
#include "../Graphics/Zone.h"
#include "../IO/FileSystem.h"
#include "../IO/Log.h"
#include "../RenderPipeline/ShaderConsts.h"
#include "../Resource/BinaryFile.h"
#include "../Resource/Image.h"
#include "../Resource/ResourceCache.h"
#include "../Resource/XMLFile.h"
#include "../Scene/Scene.h"
#include "../Utility/GLTFImporter.h"

#include <tiny_gltf.h>

#include <EASTL/optional.h>
#include <EASTL/unordered_set.h>
#include <EASTL/unordered_map.h>

#include <exception>

#include "../DebugNew.h"

namespace Urho3D
{

namespace tg = tinygltf;

namespace
{

template <class T>
struct StaticCaster
{
    template <class U>
    T operator() (U x) const { return static_cast<T>(x); }
};

template <class T, unsigned N, class U>
ea::array<T, N> ToArray(const U& vec)
{
    ea::array<T, N> result{};
    if (vec.size() >= N)
        ea::transform(vec.begin(), vec.begin() + N, result.begin(), StaticCaster<T>{});
    return result;
}

class GLTFImporterContext : public NonCopyable
{
public:
    GLTFImporterContext(Context* context, tg::Model model,
        const ea::string& outputPath, const ea::string& resourceNamePrefix)
        : context_(context)
        , model_(ea::move(model))
        , outputPath_(outputPath)
        , resourceNamePrefix_(resourceNamePrefix)
    {
    }

    ea::string CreateLocalResourceName(const ea::string& nameHint,
        const ea::string& prefix, const ea::string& defaultName, const ea::string& suffix)
    {
        const ea::string body = !nameHint.empty() ? nameHint : defaultName;
        for (unsigned i = 0; i < 1024; ++i)
        {
            const ea::string_view nameFormat = i != 0 ? "{0}{1}_{2}{3}" : "{0}{1}{3}";
            const ea::string localResourceName = Format(nameFormat, prefix, body, i, suffix);
            if (localResourceNames_.contains(localResourceName))
                continue;

            localResourceNames_.emplace(localResourceName);
            return localResourceName;
        }

        // Should never happen
        throw RuntimeException("Cannot assign resource name");
    }

    ea::string CreateResourceName(const ea::string& localResourceName)
    {
        const ea::string resourceName = resourceNamePrefix_ + localResourceName;
        const ea::string absoluteFileName = outputPath_ + localResourceName;
        resourceNameToAbsoluteFileName_[resourceName] = absoluteFileName;
        return resourceName;
    }

    ea::string GetResourceName(const ea::string& nameHint,
        const ea::string& prefix, const ea::string& defaultName, const ea::string& suffix)
    {
        const ea::string localResourceName = CreateLocalResourceName(nameHint, prefix, defaultName, suffix);
        return CreateResourceName(localResourceName);
    }

    const ea::string& GetAbsoluteFileName(const ea::string& resourceName)
    {
        const auto iter = resourceNameToAbsoluteFileName_.find(resourceName);
        return iter != resourceNameToAbsoluteFileName_.end() ? iter->second : EMPTY_STRING;
    }

    void AddToResourceCache(Resource* resource)
    {
        auto cache = context_->GetSubsystem<ResourceCache>();
        cache->AddManualResource(resource);
    }

    void SaveResource(Resource* resource)
    {
        const ea::string& fileName = GetAbsoluteFileName(resource->GetName());
        if (fileName.empty())
            throw RuntimeException("Cannot save imported resource");
        resource->SaveFile(fileName);
    }

    void SaveResource(Scene* scene)
    {
        XMLFile xmlFile(scene->GetContext());
        XMLElement rootElement = xmlFile.GetOrCreateRoot("scene");
        scene->SaveXML(rootElement);
        xmlFile.SaveFile(scene->GetFileName());
    }

    const tg::Model& GetModel() const { return model_; }
    Context* GetContext() const { return context_; }

    void CheckAccessor(int index) const { CheckT(index, model_.accessors, "Invalid accessor #{} referenced"); }
    void CheckBufferView(int index) const { CheckT(index, model_.bufferViews, "Invalid buffer view #{} referenced"); }
    void CheckImage(int index) const { CheckT(index, model_.images, "Invalid image #{} referenced"); }
    void CheckSampler(int index) const { CheckT(index, model_.samplers, "Invalid sampler #{} referenced"); }

private:
    template <class T>
    void CheckT(int index, const T& container, const char* message) const
    {
        if (index < 0 || index >= container.size())
            throw RuntimeException(message, index);
    }

    Context* const context_{};
    const tg::Model model_;
    const ea::string outputPath_;
    const ea::string resourceNamePrefix_;

    ea::unordered_set<ea::string> localResourceNames_;
    ea::unordered_map<ea::string, ea::string> resourceNameToAbsoluteFileName_;
};

class GLTFBufferReader : public NonCopyable
{
public:
    explicit GLTFBufferReader(GLTFImporterContext* context)
        : context_(context)
        , model_(context_->GetModel())
    {
    }

    template <class T>
    ea::vector<T> ReadBufferView(int bufferViewIndex, int byteOffset, int componentType, int type, int count) const
    {
        context_->CheckBufferView(bufferViewIndex);

        const int numComponents = tg::GetNumComponentsInType(type);
        if (numComponents <= 0)
            throw RuntimeException("Unexpected type {} of buffer view elements", type);

        const tg::BufferView& bufferView = model_.bufferViews[bufferViewIndex];

        ea::vector<T> result(count * numComponents);
        switch (componentType)
        {
        case TINYGLTF_COMPONENT_TYPE_BYTE:
            ReadBufferViewImpl<signed char>(result, bufferView, byteOffset, componentType, type, count);
            break;

        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
            ReadBufferViewImpl<unsigned char>(result, bufferView, byteOffset, componentType, type, count);
            break;

        case TINYGLTF_COMPONENT_TYPE_SHORT:
            ReadBufferViewImpl<short>(result, bufferView, byteOffset, componentType, type, count);
            break;

        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
            ReadBufferViewImpl<unsigned short>(result, bufferView, byteOffset, componentType, type, count);
            break;

        case TINYGLTF_COMPONENT_TYPE_INT:
            ReadBufferViewImpl<int>(result, bufferView, byteOffset, componentType, type, count);
            break;

        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
            ReadBufferViewImpl<unsigned>(result, bufferView, byteOffset, componentType, type, count);
            break;

        case TINYGLTF_COMPONENT_TYPE_FLOAT:
            ReadBufferViewImpl<float>(result, bufferView, byteOffset, componentType, type, count);
            break;

        case TINYGLTF_COMPONENT_TYPE_DOUBLE:
            ReadBufferViewImpl<double>(result, bufferView, byteOffset, componentType, type, count);
            break;

        default:
            throw RuntimeException("Unsupported component type {} of buffer view elements", componentType);
        }

        return result;
    }

    template <class T>
    ea::vector<T> ReadAccessorChecked(const tg::Accessor& accessor) const
    {
        const auto result = ReadAccessor<T>(accessor);
        if (result.size() != accessor.count)
            throw RuntimeException("Unexpected number of objects in accessor");
        return result;
    }

    template <class T>
    ea::vector<T> ReadAccessor(const tg::Accessor& accessor) const
    {
        const int numComponents = tg::GetNumComponentsInType(accessor.type);
        if (numComponents <= 0)
            throw RuntimeException("Unexpected type {} of buffer view elements", accessor.type);


        // Read dense buffer data
        ea::vector<T> result;
        if (accessor.bufferView >= 0)
        {
            result = ReadBufferView<T>(accessor.bufferView, accessor.byteOffset,
                accessor.componentType, accessor.type, accessor.count);
        }
        else
        {
            result.resize(accessor.count * numComponents);
        }

        // Read sparse buffer data
        const int numSparseElements = accessor.sparse.count;
        if (accessor.sparse.isSparse && numSparseElements > 0)
        {
            const auto& accessorIndices = accessor.sparse.indices;
            const auto& accessorValues = accessor.sparse.values;

            const auto indices = ReadBufferView<unsigned>(accessorIndices.bufferView, accessorIndices.byteOffset,
                accessorIndices.componentType, TINYGLTF_TYPE_SCALAR, numSparseElements);

            const auto values = ReadBufferView<T>(accessorValues.bufferView, accessorValues.byteOffset,
                accessor.componentType, accessor.type, numSparseElements);

            for (unsigned i = 0; i < indices.size(); ++i)
                ea::copy_n(&values[i * numComponents], numComponents, &result[indices[i] * numComponents]);
        }

        return result;
    }

private:
    static int GetByteStride(const tg::BufferView& bufferViewObject, int componentType, int type)
    {
        const int componentSizeInBytes = tg::GetComponentSizeInBytes(static_cast<uint32_t>(componentType));
        const int numComponents = tg::GetNumComponentsInType(static_cast<uint32_t>(type));
        if (componentSizeInBytes <= 0 || numComponents <= 0)
            return -1;

        return bufferViewObject.byteStride == 0
            ? componentSizeInBytes * numComponents
            : static_cast<int>(bufferViewObject.byteStride);
    }

    template <class T, class U>
    void ReadBufferViewImpl(ea::vector<U>& result,
        const tg::BufferView& bufferView, int byteOffset, int componentType, int type, int count) const
    {
        const tg::Buffer& buffer = model_.buffers[bufferView.buffer];

        const auto* bufferViewData = buffer.data.data() + bufferView.byteOffset + byteOffset;
        const int stride = GetByteStride(bufferView, componentType, type);

        const int numComponents = tg::GetNumComponentsInType(type);
        for (unsigned i = 0; i < count; ++i)
        {
            for (unsigned j = 0; j < numComponents; ++j)
            {
                T elementValue{};
                memcpy(&elementValue, bufferViewData + sizeof(T) * j, sizeof(T));
                result[i * numComponents + j] = static_cast<U>(elementValue);
            }
            bufferViewData += stride;
        }
    }

    template <class T>
    static ea::vector<T> RepackFloats(const ea::vector<float>& source)
    {
        static constexpr unsigned numComponents = sizeof(T) / sizeof(float);
        if (source.size() % numComponents != 0)
            throw RuntimeException("Unexpected number of components in array");

        const unsigned numElements = source.size() / numComponents;

        ea::vector<T> result;
        result.resize(numElements);
        for (unsigned i = 0; i < numElements; ++i)
            std::memcpy(&result[i], &source[i * numComponents], sizeof(T));
        return result;
    }

    const GLTFImporterContext* context_{};
    const tg::Model& model_;
};

template <>
ea::vector<Vector2> GLTFBufferReader::ReadAccessor(const tg::Accessor& accessor) const { return RepackFloats<Vector2>(ReadAccessor<float>(accessor)); }

template <>
ea::vector<Vector3> GLTFBufferReader::ReadAccessor(const tg::Accessor& accessor) const { return RepackFloats<Vector3>(ReadAccessor<float>(accessor)); }

template <>
ea::vector<Vector4> GLTFBufferReader::ReadAccessor(const tg::Accessor& accessor) const { return RepackFloats<Vector4>(ReadAccessor<float>(accessor)); }

class GLTFTextureImporter : public NonCopyable
{
public:
    explicit GLTFTextureImporter(GLTFImporterContext* context)
        : context_(context)
    {
        const tg::Model& model = context->GetModel();
        const unsigned numTextures = model.textures.size();
        texturesAsIs_.resize(numTextures);
        for (unsigned i = 0; i < numTextures; ++i)
            texturesAsIs_[i] = ImportTexture(i, model.textures[i]);
    }

    void CookTextures()
    {
        if (texturesCooked_)
            throw RuntimeException("Textures are already cooking");

        texturesCooked_ = true;
        for (auto& [indices, texture] : texturesMRO_)
        {
            const auto [metallicRoughnessTextureIndex, occlusionTextureIndex] = indices;

            texture.repackedImage_ = ImportRMOTexture(metallicRoughnessTextureIndex, occlusionTextureIndex,
                texture.fakeTexture_->GetName());
        }
    }

    void SaveResources()
    {
        for (const ImportedTexture& texture : texturesAsIs_)
        {
            if (!texture.isReferenced_)
                continue;
            context_->SaveResource(texture.image_);
            if (auto xmlFile = texture.cookedSamplerParams_)
                xmlFile->SaveFile(xmlFile->GetAbsoluteFileName());
        }

        for (const auto& elem : texturesMRO_)
        {
            const ImportedRMOTexture& texture = elem.second;
            context_->SaveResource(texture.repackedImage_);
            if (auto xmlFile = texture.cookedSamplerParams_)
                xmlFile->SaveFile(xmlFile->GetAbsoluteFileName());
        }
    }

    SharedPtr<Texture2D> ReferenceTextureAsIs(int textureIndex)
    {
        if (texturesCooked_)
            throw RuntimeException("Cannot reference textures after cooking");

        if (textureIndex >= texturesAsIs_.size())
            throw RuntimeException("Invalid texture #{} is referenced", textureIndex);

        ImportedTexture& texture = texturesAsIs_[textureIndex];
        texture.isReferenced_ = true;
        return texture.fakeTexture_;
    }

    SharedPtr<Texture2D> ReferenceRoughnessMetallicOcclusionTexture(
        int metallicRoughnessTextureIndex, int occlusionTextureIndex)
    {
        if (texturesCooked_)
            throw RuntimeException("Cannot reference textures after cooking");

        if (metallicRoughnessTextureIndex < 0 && occlusionTextureIndex < 0)
            throw RuntimeException("At least one texture should be referenced");
        if (metallicRoughnessTextureIndex >= 0 && metallicRoughnessTextureIndex >= texturesAsIs_.size())
            throw RuntimeException("Invalid metallic-roughness texture #{} is referenced", metallicRoughnessTextureIndex);
        if (occlusionTextureIndex >= 0 && occlusionTextureIndex >= texturesAsIs_.size())
            throw RuntimeException("Invalid occlusion texture #{} is referenced", occlusionTextureIndex);

        const auto key = ea::make_pair(metallicRoughnessTextureIndex, occlusionTextureIndex);
        const auto partialKeyA = ea::make_pair(metallicRoughnessTextureIndex, -1);
        const auto partialKeyB = ea::make_pair(-1, occlusionTextureIndex);

        // Try to find exact match
        auto iter = texturesMRO_.find(key);
        if (iter != texturesMRO_.end())
            return iter->second.fakeTexture_;

        // Try to re-purpose partial match A
        iter = texturesMRO_.find(partialKeyA);
        if (iter != texturesMRO_.end())
        {
            assert(occlusionTextureIndex != -1);
            const ImportedRMOTexture result = iter->second;
            texturesMRO_.erase(iter);
            texturesMRO_.emplace(key, result);
            return result.fakeTexture_;
        }

        // Try to re-purpose partial match B
        iter = texturesMRO_.find(partialKeyB);
        if (iter != texturesMRO_.end())
        {
            assert(metallicRoughnessTextureIndex != -1);
            const ImportedRMOTexture result = iter->second;
            texturesMRO_.erase(iter);
            texturesMRO_.emplace(key, result);
            return result.fakeTexture_;
        }

        // Create new texture
        const ImportedTexture& referenceTexture = metallicRoughnessTextureIndex >= 0
            ? texturesAsIs_[metallicRoughnessTextureIndex]
            : texturesAsIs_[occlusionTextureIndex];

        const ea::string imageName = context_->GetResourceName(
            referenceTexture.nameHint_, "Textures/", "Texture", ".png");

        ImportedRMOTexture& result = texturesMRO_[key];
        result.fakeTexture_ = MakeShared<Texture2D>(context_->GetContext());
        result.fakeTexture_->SetName(imageName);
        result.cookedSamplerParams_ = CookSamplerParams(result.fakeTexture_, referenceTexture.samplerParams_);
        return result.fakeTexture_;
    }

    static bool LoadImageData(tg::Image* image, const int imageIndex, std::string*, std::string*,
        int reqWidth, int reqHeight, const unsigned char* bytes, int size, void*)
    {
        image->name = GetFileName(image->uri.c_str()).c_str();
        image->as_is = true;
        image->image.resize(size);
        ea::copy_n(bytes, size, image->image.begin());
        return true;
    }

private:
    struct SamplerParams
    {
        TextureFilterMode filterMode_{ FILTER_DEFAULT };
        bool mipmaps_{ true };
        TextureAddressMode wrapU_{ ADDRESS_WRAP };
        TextureAddressMode wrapV_{ ADDRESS_WRAP };
    };

    struct ImportedTexture
    {
        bool isReferenced_{};

        ea::string nameHint_;
        SharedPtr<BinaryFile> image_;
        SharedPtr<Texture2D> fakeTexture_;
        SamplerParams samplerParams_;
        SharedPtr<XMLFile> cookedSamplerParams_;
    };

    struct ImportedRMOTexture
    {
        SharedPtr<Texture2D> fakeTexture_;
        SharedPtr<XMLFile> cookedSamplerParams_;

        SharedPtr<Image> repackedImage_;
    };

    static TextureFilterMode GetFilterMode(const tg::Sampler& sampler)
    {
        if (sampler.minFilter == -1 || sampler.magFilter == -1)
            return FILTER_DEFAULT;
        else if (sampler.magFilter == TINYGLTF_TEXTURE_FILTER_NEAREST)
        {
            if (sampler.minFilter == TINYGLTF_TEXTURE_FILTER_NEAREST
                || sampler.minFilter == TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST)
                return FILTER_NEAREST;
            else
                return FILTER_NEAREST_ANISOTROPIC;
        }
        else
        {
            if (sampler.minFilter == TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_NEAREST)
                return FILTER_BILINEAR;
            else
                return FILTER_DEFAULT;
        }
    }

    static bool HasMipmaps(const tg::Sampler& sampler)
    {
        return sampler.minFilter == -1 || sampler.magFilter == -1
            || sampler.minFilter == TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST
            || sampler.minFilter == TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_NEAREST
            || sampler.minFilter == TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_LINEAR
            || sampler.minFilter == TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR;
    }

    static TextureAddressMode GetAddressMode(int sourceMode)
    {
        switch (sourceMode)
        {
        case TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE:
            return ADDRESS_CLAMP;

        case TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT:
            return ADDRESS_MIRROR;

        case TINYGLTF_TEXTURE_WRAP_REPEAT:
        default:
            return ADDRESS_WRAP;
        }
    }

    SharedPtr<BinaryFile> ImportImageAsIs(unsigned imageIndex, const tg::Image& sourceImage) const
    {
        auto image = MakeShared<BinaryFile>(context_->GetContext());
        const ea::string imageUri = sourceImage.uri.c_str();

        if (sourceImage.mimeType == "image/jpeg" || imageUri.ends_with(".jpg") || imageUri.ends_with(".jpeg"))
        {
            const ea::string imageName = context_->GetResourceName(
                sourceImage.name.c_str(), "Textures/", "Texture", ".jpg");
            image->SetName(imageName);
        }
        else if (sourceImage.mimeType == "image/png" || imageUri.ends_with(".png"))
        {
            const ea::string imageName = context_->GetResourceName(
                sourceImage.name.c_str(), "Textures/", "Texture", ".png");
            image->SetName(imageName);
        }
        else
        {
            throw RuntimeException("Image #{} '{}' has unknown type '{}'",
                imageIndex, sourceImage.name.c_str(), sourceImage.mimeType.c_str());
        }

        ByteVector imageBytes;
        imageBytes.resize(sourceImage.image.size());
        ea::copy(sourceImage.image.begin(), sourceImage.image.end(), imageBytes.begin());
        image->SetData(imageBytes);
        return image;
    }

    SharedPtr<Image> DecodeImage(BinaryFile* imageAsIs) const
    {
        Deserializer& deserializer = imageAsIs->AsDeserializer();
        deserializer.Seek(0);

        auto decodedImage = MakeShared<Image>(context_->GetContext());
        decodedImage->SetName(imageAsIs->GetName());
        decodedImage->Load(deserializer);
        return decodedImage;
    }

    ImportedTexture ImportTexture(unsigned textureIndex, const tg::Texture& sourceTexture) const
    {
        const tg::Model& model = context_->GetModel();
        context_->CheckImage(sourceTexture.source);

        const tg::Image& sourceImage = model.images[sourceTexture.source];

        ImportedTexture result;
        result.nameHint_ = sourceImage.name.c_str();
        result.image_ = ImportImageAsIs(sourceTexture.source, sourceImage);
        result.fakeTexture_ = MakeShared<Texture2D>(context_->GetContext());
        result.fakeTexture_->SetName(result.image_->GetName());
        if (sourceTexture.sampler >= 0)
        {
            context_->CheckSampler(sourceTexture.sampler);

            const tg::Sampler& sourceSampler = model.samplers[sourceTexture.sampler];
            result.samplerParams_.filterMode_ = GetFilterMode(sourceSampler);
            result.samplerParams_.mipmaps_ = HasMipmaps(sourceSampler);
            result.samplerParams_.wrapU_ = GetAddressMode(sourceSampler.wrapS);
            result.samplerParams_.wrapV_ = GetAddressMode(sourceSampler.wrapT);
        }
        result.cookedSamplerParams_ = CookSamplerParams(result.image_, result.samplerParams_);
        return result;
    }

    SharedPtr<XMLFile> CookSamplerParams(Resource* image, const SamplerParams& samplerParams) const
    {
        static const ea::string addressModeNames[] =
        {
            "wrap",
            "mirror",
            "",
            "border"
        };

        static const ea::string filterModeNames[] =
        {
            "nearest",
            "bilinear",
            "trilinear",
            "anisotropic",
            "nearestanisotropic",
            "default"
        };

        auto xmlFile = MakeShared<XMLFile>(context_->GetContext());

        XMLElement rootElement = xmlFile->CreateRoot("texture");

        if (samplerParams.wrapU_ != ADDRESS_WRAP)
        {
            XMLElement childElement = rootElement.CreateChild("address");
            childElement.SetAttribute("coord", "u");
            childElement.SetAttribute("mode", addressModeNames[samplerParams.wrapU_]);
        };

        if (samplerParams.wrapV_ != ADDRESS_WRAP)
        {
            XMLElement childElement = rootElement.CreateChild("address");
            childElement.SetAttribute("coord", "v");
            childElement.SetAttribute("mode", addressModeNames[samplerParams.wrapV_]);
        };

        if (samplerParams.filterMode_ != FILTER_DEFAULT)
        {
            XMLElement childElement = rootElement.CreateChild("filter");
            childElement.SetAttribute("mode", filterModeNames[samplerParams.filterMode_]);
        }

        if (!samplerParams.mipmaps_)
        {
            XMLElement childElement = rootElement.CreateChild("mipmap");
            childElement.SetBool("enable", false);
        }

        // Don't create XML if all parameters are default
        if (!rootElement.GetChild())
            return nullptr;

        const ea::string& imageName = image->GetName();
        xmlFile->SetName(ReplaceExtension(imageName, ".xml"));
        xmlFile->SetAbsoluteFileName(ReplaceExtension(context_->GetAbsoluteFileName(imageName), ".xml"));
        return xmlFile;
    }

    SharedPtr<Image> ImportRMOTexture(
        int metallicRoughnessTextureIndex, int occlusionTextureIndex, const ea::string& name)
    {
        // Unpack input images
        SharedPtr<Image> metallicRoughnessImage = metallicRoughnessTextureIndex >= 0
            ? DecodeImage(texturesAsIs_[metallicRoughnessTextureIndex].image_)
            : nullptr;

        SharedPtr<Image> occlusionImage = occlusionTextureIndex >= 0
            ? DecodeImage(texturesAsIs_[occlusionTextureIndex].image_)
            : nullptr;

        if (!metallicRoughnessImage && !occlusionImage)
        {
            throw RuntimeException("Neither metallic-roughness texture #{} nor occlusion texture #{} can be loaded",
                metallicRoughnessTextureIndex, occlusionTextureIndex);
        }

        const IntVector3 metallicRoughnessImageSize = metallicRoughnessImage ? metallicRoughnessImage->GetSize() : IntVector3::ZERO;
        const IntVector3 occlusionImageSize = occlusionImage ? occlusionImage->GetSize() : IntVector3::ZERO;
        const IntVector2 repackedImageSize = VectorMax(metallicRoughnessImageSize.ToVector2(), occlusionImageSize.ToVector2());

        if (repackedImageSize.x_ <= 0 || repackedImageSize.y_ <= 0)
            throw RuntimeException("Repacked metallic-roughness-occlusion texture has invalid size");

        if (metallicRoughnessImage && metallicRoughnessImageSize.ToVector2() != repackedImageSize)
            metallicRoughnessImage->Resize(repackedImageSize.x_, repackedImageSize.y_);

        if (occlusionImage && occlusionImageSize.ToVector2() != repackedImageSize)
            occlusionImage->Resize(repackedImageSize.x_, repackedImageSize.y_);

        auto finalImage = MakeShared<Image>(context_->GetContext());
        finalImage->SetName(name);
        finalImage->SetSize(repackedImageSize.x_, repackedImageSize.y_, 1, occlusionImage ? 4 : 3);

        for (const IntVector2 texel : IntRect{ IntVector2::ZERO, repackedImageSize })
        {
            // 0xOO__MMRR
            unsigned color{};
            if (metallicRoughnessImage)
            {
                // 0x__MMRR__
                const unsigned value = metallicRoughnessImage->GetPixelInt(texel.x_, texel.y_);
                color |= (value >> 8) & 0xffff;
            }
            if (occlusionImage)
            {
                // 0x______OO
                const unsigned value = occlusionImage->GetPixelInt(texel.x_, texel.y_);
                color |= (value & 0xff) << 24;
            }
            else
            {
                color |= 0xff000000;
            }
            finalImage->SetPixelInt(texel.x_, texel.y_, color);
        }

        return finalImage;
    }

    GLTFImporterContext* context_{};
    ea::vector<ImportedTexture> texturesAsIs_;
    ea::unordered_map<ea::pair<int, int>, ImportedRMOTexture> texturesMRO_;

    bool texturesCooked_{};
};

class GLTFMaterialImporter : public NonCopyable
{
public:
    explicit GLTFMaterialImporter(GLTFImporterContext* context, GLTFTextureImporter* textureImporter)
        : context_(context)
        , textureImporter_(textureImporter)
    {
        // Materials are imported on-demand
    }

    SharedPtr<Material> GetOrImportMaterial(const tg::Material& sourceMaterial, const ModelVertexFormat& vertexFormat)
    {
        const ImportedMaterialKey key{ &sourceMaterial, GetImportMaterialFlags(vertexFormat) };
        SharedPtr<Material>& material = materials_[key];
        if (!material)
        {
            auto cache = context_->GetContext()->GetSubsystem<ResourceCache>();

            material = MakeShared<Material>(context_->GetContext());

            const tg::PbrMetallicRoughness& pbr = sourceMaterial.pbrMetallicRoughness;
            const Vector4 baseColor{ ToArray<float, 4>(pbr.baseColorFactor).data() };
            material->SetShaderParameter(ShaderConsts::Material_MatDiffColor, baseColor);
            material->SetShaderParameter(ShaderConsts::Material_Metallic, static_cast<float>(pbr.metallicFactor));
            material->SetShaderParameter(ShaderConsts::Material_Roughness, static_cast<float>(pbr.roughnessFactor));

            const ea::string techniqueName = "Techniques/LitOpaque.xml";
            auto technique = cache->GetResource<Technique>(techniqueName);
            if (!technique)
            {
                throw RuntimeException("Cannot find standard technique '{}' for material '{}'",
                    techniqueName, sourceMaterial.name.c_str());
            }

            material->SetTechnique(0, technique);
            material->SetVertexShaderDefines("PBR");
            material->SetPixelShaderDefines("PBR");

            if (pbr.baseColorTexture.index >= 0)
            {
                if (pbr.baseColorTexture.texCoord != 0)
                {
                    URHO3D_LOGWARNING("Material '{}' has non-standard UV for diffuse texture #{}",
                        sourceMaterial.name.c_str(), pbr.baseColorTexture.index);
                }

                const SharedPtr<Texture2D> diffuseTexture = textureImporter_->ReferenceTextureAsIs(
                    pbr.baseColorTexture.index);
                material->SetTexture(TU_DIFFUSE, diffuseTexture);
            }

            // Occlusion and metallic-roughness textures are backed together,
            // ignore occlusion if is uses different UV.
            int occlusionTextureIndex = sourceMaterial.occlusionTexture.index;
            int metallicRoughnessTextureIndex = pbr.metallicRoughnessTexture.index;
            if (occlusionTextureIndex >= 0 && metallicRoughnessTextureIndex >= 0
                && sourceMaterial.occlusionTexture.texCoord != pbr.metallicRoughnessTexture.texCoord)
            {
                URHO3D_LOGWARNING("Material '{}' uses different UV for metallic-roughness texture #{} "
                    "and for occlusion texture #{}. Occlusion texture is ignored.",
                    sourceMaterial.name.c_str(), metallicRoughnessTextureIndex, occlusionTextureIndex);
                occlusionTextureIndex = -1;
            }

            if (metallicRoughnessTextureIndex >= 0 || occlusionTextureIndex >= 0)
            {
                if (metallicRoughnessTextureIndex >= 0 && pbr.metallicRoughnessTexture.texCoord != 0)
                {
                    URHO3D_LOGWARNING("Material '{}' has non-standard UV for metallic-roughness texture #{}",
                        sourceMaterial.name.c_str(), metallicRoughnessTextureIndex);
                }

                if (occlusionTextureIndex >= 0)
                {
                    if (sourceMaterial.occlusionTexture.texCoord != 0)
                    {
                        URHO3D_LOGWARNING("Material '{}' has non-standard UV for occlusion texture #{}",
                            sourceMaterial.name.c_str(), occlusionTextureIndex);
                    }
                    if (sourceMaterial.occlusionTexture.strength != 1.0)
                    {
                        URHO3D_LOGWARNING("Material '{}' has non-default occlusion strength for occlusion texture #{}",
                            sourceMaterial.name.c_str(), occlusionTextureIndex);
                    }
                }

                const SharedPtr<Texture2D> metallicRoughnessTexture = textureImporter_->ReferenceRoughnessMetallicOcclusionTexture(
                    metallicRoughnessTextureIndex, occlusionTextureIndex);
                material->SetTexture(TU_SPECULAR, metallicRoughnessTexture);
            }

            const ea::string materialName = context_->GetResourceName(
                sourceMaterial.name.c_str(), "Materials/", "Material", ".xml");
            material->SetName(materialName);

            context_->AddToResourceCache(material);
        }
        return material;
    }

    void SaveResources()
    {
        for (const auto& item : materials_)
            context_->SaveResource(item.second);
    }

private:
    enum ImportedMaterialFlag
    {
    };

    using ImportedMaterialKey = ea::pair<const tg::Material*, unsigned>;

    unsigned GetImportMaterialFlags(const ModelVertexFormat& vertexFormat) const
    {
        unsigned flags{};
        return flags;
    }

    GLTFImporterContext* context_{};
    GLTFTextureImporter* textureImporter_{};
    ea::unordered_map<ImportedMaterialKey, SharedPtr<Material>> materials_;
};

tg::Model LoadGLTF(const ea::string& fileName)
{
    tg::TinyGLTF loader;
    loader.SetImageLoader(&GLTFTextureImporter::LoadImageData, nullptr);

    std::string errorMessage;
    tg::Model model;
    if (!loader.LoadASCIIFromFile(&model, &errorMessage, nullptr, fileName.c_str()))
        throw RuntimeException("Failed to import GLTF file: {}", errorMessage.c_str());

    return model;
}

}

class GLTFImporter::Impl
{
public:
    explicit Impl(Context* context, const ea::string& fileName,
        const ea::string& outputPath, const ea::string& resourceNamePrefix)
        : context_(context)
        , importerContext_(context, LoadGLTF(fileName), outputPath, resourceNamePrefix)
        , bufferReader_(&importerContext_)
        , textureImporter_(&importerContext_)
        , materialImporter_(&importerContext_, &textureImporter_)
    {
        // TODO: Remove me
        model_ = importerContext_.GetModel();

        ImportMeshesAndMaterials();
    }

    bool CookResources()
    {
        textureImporter_.CookTextures();
        auto cache = context_->GetSubsystem<ResourceCache>();

        for (ModelView* modelView : meshToModelView_)
        {
            const auto model = modelView ? CookModelResource(modelView) : nullptr;
            meshToModel_.push_back(model);

            if (model)
            {
                cache->AddManualResource(model);
                importedModels_.push_back(model);
            }
        }

        for (const tg::Scene& sourceScene : model_.scenes)
        {
            const auto scene = ImportScene(sourceScene);
            importedScenes_.push_back(scene);
        }

        return true;
    }

    bool SaveResources()
    {
        textureImporter_.SaveResources();
        materialImporter_.SaveResources();

        for (Material* material : importedMaterials_)
            importerContext_.SaveResource(material);

        for (Model* model : importedModels_)
            importerContext_.SaveResource(model);

        for (Scene* scene : importedScenes_)
            importerContext_.SaveResource(scene);

        return true;
    }

private:
    void ImportMeshesAndMaterials()
    {
        meshToSkin_.clear();
        meshToSkin_.resize(model_.meshes.size());

        for (const tg::Node& node : model_.nodes)
        {
            if (node.mesh < 0)
                continue;

            auto& meshSkin = meshToSkin_[node.mesh];

            if (!meshSkin)
            {
                if (node.skin >= 0)
                    meshSkin = node.skin;
            }
            else
            {
                URHO3D_LOGWARNING("Mesh #{} '{}' has multiple assigned skins, skin #{} '{}' is used.",
                    node.mesh, model_.meshes[node.mesh].name.c_str(),
                    *meshSkin, model_.skins[*meshSkin].name.c_str());
            }
        }

        const unsigned numMeshes = model_.meshes.size();
        meshToModelView_.resize(numMeshes);
        meshToMaterials_.resize(numMeshes);
        for (unsigned i = 0; i < numMeshes; ++i)
        {
            auto modelView = ImportModelView(model_.meshes[i]);
            if (modelView)
                meshToMaterials_[i] = modelView->ExportMaterialList();
            meshToModelView_[i] = modelView;
        }
    }

    SharedPtr<ModelView> ImportModelView(const tg::Mesh& sourceMesh)
    {
        const ea::string modelName = importerContext_.GetResourceName(sourceMesh.name.c_str(), "", "Model", ".mdl");

        auto modelView = MakeShared<ModelView>(context_);
        modelView->SetName(modelName);

        const unsigned numMorphWeights = sourceMesh.weights.size();
        for (unsigned morphIndex = 0; morphIndex < numMorphWeights; ++morphIndex)
            modelView->SetMorph(morphIndex, { "", static_cast<float>(sourceMesh.weights[morphIndex]) });

        auto& geometries = modelView->GetGeometries();

        const unsigned numGeometries = sourceMesh.primitives.size();
        geometries.resize(numGeometries);
        for (unsigned geometryIndex = 0; geometryIndex < numGeometries; ++geometryIndex)
        {
            GeometryView& geometryView = geometries[geometryIndex];
            geometryView.lods_.resize(1);
            GeometryLODView& geometryLODView = geometryView.lods_[0];

            const tg::Primitive& primitive = sourceMesh.primitives[geometryIndex];
            if (primitive.mode != TINYGLTF_MODE_TRIANGLES)
            {
                URHO3D_LOGWARNING("Unsupported geometry type {} in mesh '{}'.", primitive.mode, sourceMesh.name.c_str());
                return nullptr;
            }

            if (primitive.attributes.empty())
            {
                URHO3D_LOGWARNING("No attributes in primitive #{} in mesh '{}'.", geometryIndex, sourceMesh.name.c_str());
                return nullptr;
            }

            const unsigned numVertices = model_.accessors[primitive.attributes.begin()->second].count;

            geometryLODView.indices_ = ReadOptionalAccessor<unsigned>(primitive.indices);
            geometryLODView.vertices_.resize(numVertices);

            for (const auto& attribute : primitive.attributes)
            {
                const tg::Accessor& accessor = model_.accessors[attribute.second];
                if (!ReadVertexData(geometryLODView.vertexFormat_, geometryLODView.vertices_,
                    attribute.first.c_str(), accessor))
                {
                    URHO3D_LOGWARNING("Cannot read primitive #{} in mesh '{}'.", geometryIndex, sourceMesh.name.c_str());
                    return nullptr;
                }
            }

            if (primitive.material >= 0)
            {
                if (auto material = materialImporter_.GetOrImportMaterial(model_.materials[primitive.material], geometryLODView.vertexFormat_))
                    geometryView.material_ = material->GetName();
            }

            if (numMorphWeights > 0 && primitive.targets.size() != numMorphWeights)
            {
                throw RuntimeException("Primitive #{} in mesh '{}' has incorrect number of morph weights.",
                    geometryIndex, sourceMesh.name.c_str());
            }

            for (unsigned morphIndex = 0; morphIndex < primitive.targets.size(); ++morphIndex)
            {
                const auto& morphAttributes = primitive.targets[morphIndex];
                geometryLODView.morphs_[morphIndex] = ReadVertexMorphs(morphAttributes, numVertices);
            }
        }

        modelView->Normalize();
        return modelView;
    }

    SharedPtr<Model> CookModelResource(ModelView* modelView)
    {
        auto model = modelView->ExportModel();
        return model;
    }

    SharedPtr<Scene> ImportScene(const tg::Scene& sourceScene)
    {
        auto cache = context_->GetSubsystem<ResourceCache>();
        const ea::string sceneName = importerContext_.GetResourceName(sourceScene.name.c_str(), "", "Scene", ".xml");

        auto scene = MakeShared<Scene>(context_);
        scene->SetFileName(importerContext_.GetAbsoluteFileName(sceneName));
        scene->CreateComponent<Octree>();

        for (int nodeIndex : sourceScene.nodes)
        {
            ImportNode(scene, model_.nodes[nodeIndex]);
        }

        if (!scene->GetComponent<Light>(true))
        {
            // Model forward is Z+, make default lighting from top right when looking at forward side of model.
            Node* node = scene->CreateChild("Default Light");
            node->SetDirection({ 1.0f, -2.0f, -1.0f });
            auto light = node->CreateComponent<Light>();
            light->SetLightType(LIGHT_DIRECTIONAL);
        }

        if (!scene->GetComponent<Zone>(true) && !scene->GetComponent<Skybox>(true))
        {
            auto skyboxMaterial = cache->GetResource<Material>("Materials/Skybox.xml");
            auto skyboxTexture = cache->GetResource<TextureCube>("Textures/Skybox.xml");
            auto boxModel = cache->GetResource<Model>("Models/Box.mdl");

            if (skyboxMaterial && skyboxTexture && boxModel)
            {
                Node* zoneNode = scene->CreateChild("Default Zone");
                auto zone = zoneNode->CreateComponent<Zone>();
                zone->SetBackgroundBrightness(0.5f);
                zone->SetZoneTexture(skyboxTexture);

                Node* skyboxNode = scene->CreateChild("Default Skybox");
                auto skybox = skyboxNode->CreateComponent<Skybox>();
                skybox->SetModel(boxModel);
                skybox->SetMaterial(skyboxMaterial);
            }

        }

        return scene;
    }

    void ExtractTransform(const tg::Node& node, Vector3& translation, Quaternion& rotation, Vector3& scale)
    {
        translation = Vector3::ZERO;
        rotation = Quaternion::IDENTITY;
        scale = Vector3::ONE;

        if (!node.matrix.empty())
        {
            Matrix4 sourceMatrix;
            ea::transform(node.matrix.begin(), node.matrix.end(),
                &sourceMatrix.m00_, StaticCaster<float>{});

            const Matrix3x4 transform{ sourceMatrix.Transpose() };
            transform.Decompose(translation, rotation, scale);
        }
        else
        {
            if (!node.translation.empty())
            {
                ea::transform(node.translation.begin(), node.translation.end(),
                    &translation.x_, StaticCaster<float>{});
            }
            if (!node.rotation.empty())
            {
                ea::transform(node.rotation.begin(), node.rotation.end(),
                    &rotation.w_, StaticCaster<float>{});
            }
            if (!node.scale.empty())
            {
                ea::transform(node.scale.begin(), node.scale.end(),
                    &scale.x_, StaticCaster<float>{});
            }
        }
    }

    void ImportNode(Node* parent, const tg::Node& sourceNode)
    {
        auto cache = context_->GetSubsystem<ResourceCache>();

        Node* node = parent->CreateChild(sourceNode.name.c_str());

        Vector3 translation;
        Quaternion rotation;
        Vector3 scale;
        ExtractTransform(sourceNode, translation, rotation, scale);
        node->SetTransform(translation, rotation, scale);

        if (sourceNode.mesh >= 0)
        {
            if (Model* model = meshToModel_[sourceNode.mesh])
            {
                const bool needAnimation = model->GetNumMorphs() > 0 || model->GetSkeleton().GetNumBones() > 1;
                auto staticModel = !needAnimation
                    ? node->CreateComponent<StaticModel>()
                    : node->CreateComponent<AnimatedModel>();

                staticModel->SetModel(model);

                const StringVector& meshMaterials = meshToMaterials_[sourceNode.mesh];
                for (unsigned i = 0; i < meshMaterials.size(); ++i)
                {
                    auto material = cache->GetResource<Material>(meshMaterials[i]);
                    staticModel->SetMaterial(i, material);
                }
            }
        }

        for (int childIndex : sourceNode.children)
        {
            ImportNode(node, model_.nodes[childIndex]);
        }
    }

    template <class T>
    ea::vector<T> ReadOptionalAccessor(int accessorIndex) const
    {
        ea::vector<T> result;
        if (accessorIndex >= 0)
        {
            const tg::Accessor& accessor = model_.accessors[accessorIndex];
            result = bufferReader_.ReadAccessor<T>(accessor);
        }
        return result;
    }

    bool ReadVertexData(ModelVertexFormat& vertexFormat, ea::vector<ModelVertex>& vertices,
        const ea::string& semantics, const tg::Accessor& accessor)
    {
        const auto& parsedSemantics = semantics.split('_');
        const ea::string& semanticsName = parsedSemantics[0];
        const unsigned semanticsIndex = parsedSemantics.size() > 1 ? FromString<unsigned>(parsedSemantics[1]) : 0;

        if (semanticsName == "POSITION")
        {
            if (accessor.type != TINYGLTF_TYPE_VEC3)
            {
                URHO3D_LOGERROR("Unexpected type of vertex position");
                return false;
            }

            vertexFormat.position_ = TYPE_VECTOR3;

            const auto positions = bufferReader_.ReadAccessorChecked<Vector3>(accessor);
            for (unsigned i = 0; i < accessor.count; ++i)
                vertices[i].SetPosition(positions[i]);
        }
        else if (semanticsName == "NORMAL")
        {
            if (accessor.type != TINYGLTF_TYPE_VEC3)
            {
                URHO3D_LOGERROR("Unexpected type of vertex normal");
                return false;
            }

            vertexFormat.normal_ = TYPE_VECTOR3;

            const auto normals = bufferReader_.ReadAccessorChecked<Vector3>(accessor);
            for (unsigned i = 0; i < accessor.count; ++i)
                vertices[i].SetNormal(normals[i].Normalized());
        }
        else if (semanticsName == "TANGENT")
        {
            if (accessor.type != TINYGLTF_TYPE_VEC4)
            {
                URHO3D_LOGERROR("Unexpected type of vertex tangent");
                return false;
            }

            vertexFormat.tangent_ = TYPE_VECTOR4;

            const auto tangents = bufferReader_.ReadAccessorChecked<Vector4>(accessor);
            for (unsigned i = 0; i < accessor.count; ++i)
                vertices[i].tangent_ = tangents[i];
        }
        else if (semanticsName == "TEXCOORD" && semanticsIndex < ModelVertex::MaxUVs)
        {
            if (accessor.type != TINYGLTF_TYPE_VEC2)
            {
                URHO3D_LOGERROR("Unexpected type of vertex uv");
                return false;
            }

            vertexFormat.uv_[semanticsIndex] = TYPE_VECTOR2;

            const auto uvs = bufferReader_.ReadAccessorChecked<Vector2>(accessor);
            for (unsigned i = 0; i < accessor.count; ++i)
                vertices[i].uv_[semanticsIndex] = { uvs[i], Vector2::ZERO };
        }
        else if (semanticsName == "COLOR" && semanticsIndex < ModelVertex::MaxColors)
        {
            if (accessor.type != TINYGLTF_TYPE_VEC3 && accessor.type != TINYGLTF_TYPE_VEC4)
            {
                URHO3D_LOGERROR("Unexpected type of vertex color");
                return false;
            }

            if (accessor.type == TINYGLTF_TYPE_VEC3)
            {
                vertexFormat.color_[semanticsIndex] = TYPE_VECTOR3;

                const auto colors = bufferReader_.ReadAccessorChecked<Vector3>(accessor);
                for (unsigned i = 0; i < accessor.count; ++i)
                    vertices[i].color_[semanticsIndex] = { colors[i], 1.0f };
            }
            else if (accessor.type == TINYGLTF_TYPE_VEC4)
            {
                vertexFormat.color_[semanticsIndex] = TYPE_VECTOR4;

                const auto colors = bufferReader_.ReadAccessorChecked<Vector4>(accessor);
                for (unsigned i = 0; i < accessor.count; ++i)
                    vertices[i].color_[semanticsIndex] = colors[i];
            }
        }

        return true;
    }

    ModelVertexMorphVector ReadVertexMorphs(const std::map<std::string, int>& accessors, unsigned numVertices)
    {
        ea::vector<Vector3> positionDeltas(numVertices);
        ea::vector<Vector3> normalDeltas(numVertices);
        ea::vector<Vector3> tangentDeltas(numVertices);

        if (const auto positionIter = accessors.find("POSITION"); positionIter != accessors.end())
        {
            importerContext_.CheckAccessor(positionIter->second);
            positionDeltas = bufferReader_.ReadAccessor<Vector3>(model_.accessors[positionIter->second]);
        }

        if (const auto normalIter = accessors.find("NORMAL"); normalIter != accessors.end())
        {
            importerContext_.CheckAccessor(normalIter->second);
            normalDeltas = bufferReader_.ReadAccessor<Vector3>(model_.accessors[normalIter->second]);
        }

        if (const auto tangentIter = accessors.find("TANGENT"); tangentIter != accessors.end())
        {
            importerContext_.CheckAccessor(tangentIter->second);
            tangentDeltas = bufferReader_.ReadAccessor<Vector3>(model_.accessors[tangentIter->second]);
        }

        if (numVertices != positionDeltas.size() || numVertices != normalDeltas.size() || numVertices != tangentDeltas.size())
            throw RuntimeException("Morph target has inconsistent sizes of accessors");

        ModelVertexMorphVector vertexMorphs(numVertices);
        for (unsigned i = 0; i < numVertices; ++i)
        {
            vertexMorphs[i].index_ = i;
            vertexMorphs[i].positionDelta_ = positionDeltas[i];
            vertexMorphs[i].normalDelta_ = normalDeltas[i];
            vertexMorphs[i].tangentDelta_ = tangentDeltas[i];
        }
        return vertexMorphs;
    }

    GLTFImporterContext importerContext_;
    GLTFBufferReader bufferReader_;
    GLTFTextureImporter textureImporter_;
    GLTFMaterialImporter materialImporter_;

    Context* context_{};

    /// Initialized after loading
    /// @{
    tg::Model model_;

    ea::vector<ea::optional<int>> meshToSkin_;
    ea::vector<SharedPtr<ModelView>> meshToModelView_;
    ea::vector<StringVector> meshToMaterials_;
    ea::vector<SharedPtr<Image>> texturesToImageAsIs_;
    ea::vector<SharedPtr<Texture>> texturesToFakeTextures_;
    /// @}

    /// Initialized after cooking
    /// @{
    ea::vector<SharedPtr<Model>> importedModels_;
    ea::vector<SharedPtr<Material>> importedMaterials_;
    ea::vector<SharedPtr<Model>> meshToModel_;
    ea::vector<SharedPtr<Scene>> importedScenes_;
    /// @}
};

GLTFImporter::GLTFImporter(Context* context)
    : Object(context)
{

}

GLTFImporter::~GLTFImporter()
{

}

bool GLTFImporter::LoadFile(const ea::string& fileName,
    const ea::string& outputPath, const ea::string& resourceNamePrefix)
{
    try
    {
        impl_ = ea::make_unique<Impl>(context_, fileName, outputPath, resourceNamePrefix);
        return true;
    }
    catch(const RuntimeException& e)
    {
        URHO3D_LOGERROR("{}", e.what());
        return false;
    }
}

bool GLTFImporter::CookResources()
{
    try
    {
        if (!impl_)
            throw RuntimeException("GLTF file wasn't loaded");

        return impl_->CookResources();
    }
    catch(const RuntimeException& e)
    {
        URHO3D_LOGERROR("{}", e.what());
        return false;
    }
}

bool GLTFImporter::SaveResources()
{
    try
    {
        if (!impl_)
            throw RuntimeException("Imported asserts weren't cooked");

        return impl_->SaveResources();
    }
    catch(const RuntimeException& e)
    {
        URHO3D_LOGERROR("{}", e.what());
        return false;
    }
}

}