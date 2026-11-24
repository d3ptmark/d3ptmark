#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "renderer_pt/pt_scene.h"
#ifdef PT_PHYSICAL
#include "renderer_pt/pt_material_editor.h"
#include "renderer_pt/pt_material_profiles.h"
#endif

/*
===============================================================================

	Material translation: idMaterial stages -> ptMaterial_t

===============================================================================
*/

extern idCVar r_ptDebug;

/*
========================
PT_InitMaterial
========================
*/
static void PT_InitMaterial( ptMaterial_t &mat ) {
    memset( &mat, 0, sizeof( mat ) );
    mat.albedo[0] = mat.albedo[1] = mat.albedo[2] = 0.5f;
    mat.albedo[3] = 1.0f;
    mat.roughness = 0.7f;
    mat.ior = 1.5f;
    mat.albedoTexture = -1;
    mat.normalTexture = -1;
    mat.specularTexture = -1;
    mat.emissionTexture = -1;
    mat.emissionAlpha = 1.0f;
    mat.emissionTransform[0] = mat.emissionTransform[4] = 1.0f;
    mat.additiveMaskTexture = -1;
    mat.additiveMaskAlpha = 1.0f;
    mat.additiveMaskTransform[0] = mat.additiveMaskTransform[4] = 1.0f;
    mat.blendMode = PT_BLEND_OPAQUE;
    mat.bumpScale = 1.0f;
    mat.texTransform[0] = 1.0f;
    mat.texTransform[4] = 1.0f;
}

/*
========================
idPathTracerScene::AddDefaultMaterial

Slot PT_DEFAULT_MATERIAL is reserved for surfaces without a material.
========================
*/
void idPathTracerScene::AddDefaultMaterial() {
    ptMaterial_t mat;
    PT_InitMaterial( mat );
    materials.Append( mat );
    materialEmission.Append( vec3_origin );
}

/*
========================
idPathTracerScene::GetOrAddMaterial
========================
*/
int idPathTracerScene::GetOrAddMaterial( const idMaterial *material, const float *shaderRegisters ) {
    if ( !material ) {
        return PT_DEFAULT_MATERIAL;
    }

    // Animated materials (time / entity driven registers) are evaluated per
    // register set: entities sharing a material with different shaderParms get
    // separate slots. Slots are recycled across frames, a slot claimed this
    // frame is only shared by surfaces with identical registers.
    const bool animated = !material->ConstantRegisters() && shaderRegisters;
    const unsigned int regHash = animated
        ? PT_HashWords( 2166136261u, shaderRegisters, material->GetNumRegisters() * sizeof( float ) ) : 0;

    bool rebuild;
    const int index = materialCache.Acquire( material, PT_HashString( material->GetName() ),
        animated, regHash, frameCount, rebuild );
    if ( !rebuild ) {
        return index;
    }

    if ( index < materials.Num() ) {
        BuildMaterial( materials[index], materialEmission[index], material, shaderRegisters );
    } else {
        assert( index == materials.Num() );
        ptMaterial_t mat;
        idVec3 emission;
        BuildMaterial( mat, emission, material, shaderRegisters );
        materials.Append( mat );
        materialEmission.Append( emission );
    }

    if ( r_ptDebug.GetBool() ) {
        common->Printf( "idPathTracerScene: material %d: %s\n", index, material->GetName() );
    }
    return index;
}

/*
========================
idPathTracerScene::BuildMaterial
========================
*/
void idPathTracerScene::BuildMaterial( ptMaterial_t &mat, idVec3 &emission, const idMaterial *material, const float *shaderRegisters ) {
    PT_InitMaterial( mat );
    emission.Zero();

    if ( r_ptDebug.GetBool() ) {
        common->Printf( "BuildMaterial: %s (stages=%d, regs=%d, constRegs=%s)\n",
            material->GetName(), material->GetNumStages(), material->GetNumRegisters(),
            material->ConstantRegisters() ? "yes" : "no" );
    }

    // extract coverage type and alpha test threshold from engine material
    int coverage = 0; // local: 0=opaque, 1=perforated, 2=translucent
    materialCoverage_t cov = material->Coverage();
    if (cov == MC_PERFORATED) {
        coverage = 1;
        mat.alphaTest = 0.5f; // default threshold for perforated
    } else if (cov == MC_TRANSLUCENT) {
        coverage = 2;
        mat.alphaTest = 0.0f; // translucent uses continuous alpha
    }

    // extract noShadows flag from material (SurfaceCastsShadow checks MF_NOSHADOWS and MF_FORCESHADOWS)
    if (!material->SurfaceCastsShadow()) {
        mat.noShadows = 1;
    }

    // resolve registers: constant materials carry their own, animated ones use the
    // entity's evaluated registers; fall back to 1.0 when none are available
    const float *regs = material->ConstantRegisters();
    const int numRegs = material->GetNumRegisters();
    if ( !regs ) {
        regs = shaderRegisters;
    }

    // safe register lookup helper macro
    #define SAFE_REG(regIdx) ((regs && (regIdx) >= 0 && (regIdx) < numRegs) ? regs[regIdx] : 1.0f)

    mat.decalOverlay = mat.noShadows && ( material->GetSort() == SS_DECAL
        || material->TestMaterialFlag( MF_POLYGONOFFSET ) || cov == MC_TRANSLUCENT );
    if ( material->TestMaterialFlag( MF_POLYGONOFFSET ) ) mat.polygonOffset = material->GetPolygonOffset();
    // extract textures
    const shaderStage_t *ambientBaseStage = NULL;   // first non-additive ambient stage
    const shaderStage_t *ambientGlowStage = NULL;   // first additive ambient stage
    const shaderStage_t *ambientMaskStage = NULL;
    const shaderStage_t *ambientBaseMaskStage = NULL;
    const shaderStage_t *ambientGlowMaskStage = NULL;
    const shaderStage_t *diffuseStage = NULL;
    const shaderStage_t *normalStage = NULL;
    const shaderStage_t *specularStage = NULL;
    const shaderStage_t *albedoTextureStage = NULL;
    bool hasActiveStage = false;

    for (int i = 0; i < material->GetNumStages(); i++) {
        const shaderStage_t* stage = material->GetStage(i);
		if (!stage) continue;
        if ( SAFE_REG( stage->conditionRegister ) == 0 ) continue;
        hasActiveStage = true;
        const int blendBits = stage->drawStateBits & (GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS);
        const shaderStage_t *precedingMaskStage = ambientMaskStage;
        const bool writesAmbientAlpha = stage->lighting == SL_AMBIENT
            && !(stage->drawStateBits & GLS_ALPHAMASK)
            && blendBits != (GLS_SRCBLEND_ZERO | GLS_DSTBLEND_ONE);
        if (writesAmbientAlpha) ambientMaskStage = NULL;

        // skip fragment program stages
        if (stage->newStage) {
            if (r_ptDebug.GetBool()) {
                common->Printf("  stage %d: SKIP (fragment program)\n", i);
            }
            continue;
        }

        // skip stages with non-standard texture coordinate generation
        if (stage->texture.texgen != TG_EXPLICIT) {
            if (r_ptDebug.GetBool()) {
                common->Printf("  stage %d: SKIP (texgen=%d)\n", i, (int)stage->texture.texgen);
            }
            continue;
        }

        switch (stage->lighting) {
            case SL_AMBIENT:
            {
				if (blendBits == (GLS_SRCBLEND_ZERO | GLS_DSTBLEND_ONE)) {
					break;
				}

                // maskcolor stages only write alpha
                bool isMaskColor = ((stage->drawStateBits & GLS_COLORMASK) == GLS_COLORMASK);
                if (isMaskColor) {
                    if (writesAmbientAlpha && blendBits == (GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO)) {
                        ambientMaskStage = stage;
                    }
                    if (r_ptDebug.GetBool()) {
                        common->Printf("  stage %d: maskcolor (alpha mask)\n", i);
                    }
                    break;
                }

                bool stageIsAdd = (blendBits == (GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE))
                    || (blendBits == (GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE));
                if (stageIsAdd) {
                    if (!ambientGlowStage) {
                        ambientGlowStage = stage;
                        ambientGlowMaskStage = precedingMaskStage;
                    }
                } else {
                    if (!ambientBaseStage) {
                        ambientBaseStage = stage;
                        ambientBaseMaskStage = precedingMaskStage;
                    }
                }
                break;
            }
            case SL_DIFFUSE:
            {
                int blendBits = stage->drawStateBits & (GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS);
				if (blendBits == (GLS_SRCBLEND_ZERO | GLS_DSTBLEND_ONE)) {
					break;
				}
                diffuseStage = stage;
                break;
            }
            case SL_BUMP:
            {
                normalStage = stage;
                break;
            }
            case SL_SPECULAR:
            {
                specularStage = stage;
                break;
            }
            default:
                break;
        }
    }

    mat.isAmbientOnly = !( diffuseStage || normalStage || specularStage );
#ifdef PT_PHYSICAL
    physicalMaterialInfo_t physical = material->GetPhysicalMaterial();
    const ptMaterialProfile_t profile = R_PT_MaterialProfile( material );
    const ptMaterialOverride_t *overrideValue = R_PT_MaterialOverride( material->GetName() );
    const bool physicalSurface = hasActiveStage && ( physical.enabled || profile.label || overrideValue );
#else
    const bool physicalSurface = false;
    (void)hasActiveStage;
#endif
    if ( !diffuseStage && !ambientBaseStage && !ambientGlowStage && !physicalSurface ) {
        // Alpha-write-only and unsupported stages do not supply visible PT color.
        if ( r_ptDebug.GetBool() ) {
            common->Printf( "  %s: no active supported color stage; transparent overlay\n", material->GetName() );
        }
        mat.isAmbientOnly = 1;
        mat.albedo[0] = mat.albedo[1] = mat.albedo[2] = mat.albedo[3] = 0;
        mat.blendMode = PT_BLEND_ALPHA;
        return;
    }

    bool hasDiffuse = (diffuseStage != NULL);

    // validate perforated coverage
    if (coverage == 1) {
        bool usableAlphaTest = false;
        if (diffuseStage && diffuseStage->hasAlphaTest) usableAlphaTest = true;
        if (ambientBaseStage && ambientBaseStage->hasAlphaTest) usableAlphaTest = true;
        if (ambientGlowStage && ambientGlowStage->hasAlphaTest) usableAlphaTest = true;
        if (!usableAlphaTest) {
            if (r_ptDebug.GetBool()) {
                common->Printf("  coverage=1 but no usable alpha-tested stage, downgrading to opaque\n");
            }
            coverage = 0;
            mat.alphaTest = 0.0f;
        }
    }

    if (r_ptDebug.GetBool()) {
        common->Printf("  stages: coverage=%d ambBase=%s ambGlow=%s ambMask=%s diff=%s norm=%s spec=%s\n",
            coverage,
            ambientBaseStage ? "yes" : "no",
            ambientGlowStage ? "yes" : "no",
            ambientMaskStage ? "yes" : "no",
            hasDiffuse ? "yes" : "no",
            normalStage ? "yes" : "no",
            specularStage ? "yes" : "no");
    }

    if (hasDiffuse) {
        albedoTextureStage = diffuseStage;
        mat.albedoTexture = AddTexture(diffuseStage->texture.image);
        if (mat.albedoTexture >= 0) {
            mat.albedo[0] = SAFE_REG(diffuseStage->color.registers[0]);
            mat.albedo[1] = SAFE_REG(diffuseStage->color.registers[1]);
            mat.albedo[2] = SAFE_REG(diffuseStage->color.registers[2]);
        }

        // refine alpha test threshold from stage draw state bits
        if (coverage == 1 || coverage == 2) {
            int atest = diffuseStage->drawStateBits & GLS_ATEST_BITS;
            if (atest & GLS_ATEST_GE_128) {
                mat.alphaTest = 128.0f / 255.0f; // ~0.502
            } else if (atest & GLS_ATEST_LT_128) {
                mat.alphaTest = 128.0f / 255.0f;
            } else if (atest & GLS_ATEST_EQ_255) {
                mat.alphaTest = 254.0f / 255.0f;
            }
        }

        if (r_ptDebug.GetBool()) {
            common->Printf("  %s: coverage=%d alphaTest=%.3f\n", material->GetName(), coverage, mat.alphaTest);
        }
    }

    // process glow stage (emission)
    int emissionTextureIdx = -1;
    if (ambientGlowStage && ambientGlowStage->texture.image && !mat.isAmbientOnly) {
        int emTex = AddTexture(ambientGlowStage->texture.image);
        if (emTex >= 0) {
            emissionTextureIdx = emTex;
            mat.emissionTexture = emTex;
            float emR = SAFE_REG(ambientGlowStage->color.registers[0]);
            float emG = SAFE_REG(ambientGlowStage->color.registers[1]);
            float emB = SAFE_REG(ambientGlowStage->color.registers[2]);
            mat.emission[0] = emR;
            mat.emission[1] = emG;
            mat.emission[2] = emB;
            mat.emissionUsesAlpha = (ambientGlowStage->drawStateBits & GLS_SRCBLEND_BITS) == GLS_SRCBLEND_SRC_ALPHA;
            mat.emissionAlpha = SAFE_REG(ambientGlowStage->color.registers[3]);
            mat.emissionVertexColor = ambientGlowStage->vertexColor == SVC_MODULATE ? 1
                : ambientGlowStage->vertexColor == SVC_INVERSE_MODULATE ? 2 : 0;
            if (ambientGlowStage->texture.hasMatrix && regs) {
                for (int row = 0; row < 2; row++) {
                    for (int col = 0; col < 3; col++) {
                        mat.emissionTransform[row * 3 + col] = SAFE_REG(ambientGlowStage->texture.matrix[row][col]);
                    }
                }
            }
            emission.Set(mat.emission[0], mat.emission[1], mat.emission[2]);
        }
    }

    // for materials with NO diffuse stage
    if (!hasDiffuse && mat.albedoTexture < 0) {
        const shaderStage_t *albedoSource = ambientBaseStage;
        if (!albedoSource) albedoSource = ambientGlowStage;

        if (albedoSource && albedoSource->texture.image) {
            albedoTextureStage = albedoSource;
            int ambTex = AddTexture(albedoSource->texture.image);
            if (ambTex >= 0) {
                mat.albedoTexture = ambTex;
                mat.albedo[0] = SAFE_REG(albedoSource->color.registers[0]);
                mat.albedo[1] = SAFE_REG(albedoSource->color.registers[1]);
                mat.albedo[2] = SAFE_REG(albedoSource->color.registers[2]);
                mat.albedo[3] = idMath::ClampFloat(0.0f, 1.0f, SAFE_REG(albedoSource->color.registers[3]));

                // extract blend mode from ambient stage draw state bits
                int srcBlend = albedoSource->drawStateBits & GLS_SRCBLEND_BITS;
                int dstBlend = albedoSource->drawStateBits & GLS_DSTBLEND_BITS;

                if (srcBlend == GLS_SRCBLEND_ONE && dstBlend == GLS_DSTBLEND_ONE) {
                    mat.blendMode = PT_BLEND_ADD;
                } else if (srcBlend == GLS_SRCBLEND_SRC_ALPHA && dstBlend == GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA) {
                    mat.blendMode = PT_BLEND_ALPHA;
                } else if ((srcBlend == GLS_SRCBLEND_DST_COLOR && dstBlend == GLS_DSTBLEND_ZERO)
                        || (srcBlend == GLS_SRCBLEND_ZERO && dstBlend == GLS_DSTBLEND_SRC_COLOR)) {
                    mat.blendMode = PT_BLEND_MODULATE;
                } else if (srcBlend == GLS_SRCBLEND_ZERO && dstBlend == GLS_DSTBLEND_ONE_MINUS_SRC_COLOR) {
                    mat.blendMode = PT_BLEND_MODULATE_INVERSE;
                } else if (srcBlend != GLS_SRCBLEND_ONE || dstBlend != GLS_DSTBLEND_ZERO) {
                    // Non-opaque blend mode: check for additive variant (e.g. GL_SRC_ALPHA + GL_ONE)
                    if (dstBlend == GLS_DSTBLEND_ONE) {
                        mat.blendMode = PT_BLEND_ADD;
                    } else {
                        mat.blendMode = PT_BLEND_ALPHA;
                    }
                }

                // ambient-only surfaces without a glow map get mild self-emission
                if (mat.blendMode == 0 && emissionTextureIdx < 0) {
                    mat.emission[0] = mat.albedo[0] * 0.3f;
                    mat.emission[1] = mat.albedo[1] * 0.3f;
                    mat.emission[2] = mat.albedo[2] * 0.3f;
                    emission.Set(mat.emission[0], mat.emission[1], mat.emission[2]);
                }
            }
        }
    }

    // classify blend modes for translucent materials
    {
        const shaderStage_t *blendStage = ambientBaseStage ? ambientBaseStage : ambientGlowStage;
        if (blendStage && coverage == 2) {
            int ambSrc = blendStage->drawStateBits & GLS_SRCBLEND_BITS;
            int ambDst = blendStage->drawStateBits & GLS_DSTBLEND_BITS;

            bool isAdditive = (ambSrc == GLS_SRCBLEND_ONE && ambDst == GLS_DSTBLEND_ONE);
            bool isAlphaBlend = (ambSrc == GLS_SRCBLEND_SRC_ALPHA && ambDst == GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
            bool isPremultAlpha = (ambSrc == GLS_SRCBLEND_ONE && ambDst == GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
            bool isFilter = (ambSrc == GLS_SRCBLEND_DST_COLOR && ambDst == GLS_DSTBLEND_ZERO)
                         || (ambSrc == GLS_SRCBLEND_ZERO && ambDst == GLS_DSTBLEND_SRC_COLOR);
            bool isInverseFilter = (ambSrc == GLS_SRCBLEND_ZERO && ambDst == GLS_DSTBLEND_ONE_MINUS_SRC_COLOR);

            if (isAdditive) {
                mat.blendMode = PT_BLEND_ADD;
            } else if (isAlphaBlend || isPremultAlpha) {
                mat.blendMode = PT_BLEND_ALPHA;
            } else if (isFilter) {
                mat.blendMode = PT_BLEND_MODULATE;
            } else if (isInverseFilter) {
                mat.blendMode = PT_BLEND_MODULATE_INVERSE;
            }

            // for blended surfaces, use the blend stage texture as albedo if we don't have one
            if (mat.blendMode > 0 && blendStage->texture.image && mat.albedoTexture < 0) {
                albedoTextureStage = blendStage;
                mat.albedoTexture = AddTexture(blendStage->texture.image);
                mat.albedo[0] = SAFE_REG(blendStage->color.registers[0]);
                mat.albedo[1] = SAFE_REG(blendStage->color.registers[1]);
                mat.albedo[2] = SAFE_REG(blendStage->color.registers[2]);
                mat.albedo[3] = SAFE_REG(blendStage->color.registers[3]);
            }

            if (r_ptDebug.GetBool()) {
                common->Printf("  %s: blendMode=%d\n", material->GetName(), mat.blendMode);
            }
        }
    }

    // process normal map stage
    bool hasNormal = (normalStage != NULL);
    if (hasNormal) {
        mat.normalTexture = AddTexture(normalStage->texture.image);
        if (r_ptDebug.GetBool()) {
            common->Printf("  %s: normal map %d\n", material->GetName(), mat.normalTexture);
        }
    }

    // process specular map stage
    bool hasSpecular = (specularStage != NULL);
    if (hasSpecular) {
        mat.specularTexture = AddTexture(specularStage->texture.image);
        if (r_ptDebug.GetBool()) {
            common->Printf("  %s: specular map %d\n", material->GetName(), mat.specularTexture);
        }
    }

    if (albedoTextureStage && albedoTextureStage->texture.hasMatrix && regs) {
        const int (*matrix)[3] = albedoTextureStage->texture.matrix;
        mat.texTransform[0] = SAFE_REG(matrix[0][0]);
        mat.texTransform[1] = SAFE_REG(matrix[0][1]);
        mat.texTransform[2] = SAFE_REG(matrix[0][2]);
        mat.texTransform[3] = SAFE_REG(matrix[1][0]);
        mat.texTransform[4] = SAFE_REG(matrix[1][1]);
        mat.texTransform[5] = SAFE_REG(matrix[1][2]);
    }
    if ( albedoTextureStage ) {
        mat.useVertexColor = albedoTextureStage->vertexColor == SVC_MODULATE ? 1
            : albedoTextureStage->vertexColor == SVC_INVERSE_MODULATE ? 2 : 0;
        const int srcBlend = albedoTextureStage->drawStateBits & GLS_SRCBLEND_BITS;
        mat.additiveUsesAlpha = srcBlend == GLS_SRCBLEND_SRC_ALPHA
            ? PT_ADDITIVE_SOURCE_ALPHA : PT_ADDITIVE_ONE;
        if (mat.blendMode == PT_BLEND_ADD && srcBlend == GLS_SRCBLEND_DST_ALPHA) {
            const shaderStage_t *maskStage = albedoTextureStage == ambientBaseStage ? ambientBaseMaskStage
                : albedoTextureStage == ambientGlowStage ? ambientGlowMaskStage : NULL;
            mat.additiveUsesAlpha = PT_ADDITIVE_LOCAL_MASK;
            if (maskStage) mat.additiveMaskTexture = AddTexture(maskStage->texture.image);
            if (!maskStage || mat.additiveMaskTexture < 0) {
                common->Warning("PT material '%s': DST_ALPHA additive stage has no supported resident local alpha mask; overlay omitted",
                    material->GetName());
            } else {
                mat.additiveMaskAlpha = SAFE_REG(maskStage->color.registers[3]);
                mat.additiveMaskVertexColor = maskStage->vertexColor == SVC_MODULATE ? 1
                    : maskStage->vertexColor == SVC_INVERSE_MODULATE ? 2 : 0;
                if (maskStage->texture.hasMatrix && regs) {
                    for (int row = 0; row < 2; row++) {
                        for (int col = 0; col < 3; col++) {
                            mat.additiveMaskTransform[row * 3 + col] = SAFE_REG(maskStage->texture.matrix[row][col]);
                        }
                    }
                }
            }
        }
    }

    // colors are multiplied in gamma space by the GL renderer, decode every
    // factor so that the product encoded with r_ptGamma 2.2 matches it
    for ( int c = 0; c < 3; c++ ) {
        mat.albedo[c] = PT_SrgbToLinear( mat.albedo[c] );
        mat.emission[c] = PT_SrgbToLinear( mat.emission[c] );
    }
    emission.Set( mat.emission[0], mat.emission[1], mat.emission[2] );
    if (mat.emissionUsesAlpha) {
        emission *= idMath::ClampFloat(0.0f, 1.0f, mat.emissionAlpha);
    }

#ifdef PT_PHYSICAL
    idStr effectName = material->GetName();
    effectName.ToLower();
    const bool fxMaterial = !effectName.Icmpn( "textures/particles/", 19 )
        || !effectName.Icmpn( "textures/sfx/", 13 ) || !effectName.Icmpn( "models/weapons/", 15 );
    const bool luminousFx = fxMaterial && ( effectName.Find( "flame" ) >= 0 || effectName.Find( "mflash" ) >= 0
        || effectName.Find( "spark" ) >= 0 || effectName.Find( "fire" ) >= 0 );
    mat.emissiveOverlay = mat.isAmbientOnly && ( mat.blendMode == PT_BLEND_ADD
        || ( luminousFx && mat.blendMode == PT_BLEND_ALPHA ) );
    const bool calibrated = profile.label && !overrideValue;
    if ( !overrideValue && profile.label ) overrideValue = &profile.value;
    // Untextured conductors need linear reflectance, not the gray diffuse fallback.
    if ( overrideValue && mat.albedoTexture < 0 ) {
        for ( int c = 0; c < 3; c++ ) mat.albedo[c] += ( 0.6f - mat.albedo[c] ) * overrideValue->metallic;
    }
    if ( overrideValue ) {
        physical.enabled = true;
        physical.metallic = overrideValue->metallic;
        physical.roughness = overrideValue->roughness;
        physical.transmission = overrideValue->transmission;
        physical.ior = overrideValue->ior;
        physical.thin = (int)overrideValue->thin;
        physical.thickness = overrideValue->thickness;
        for ( int c = 0; c < 3; c++ ) {
            physical.absorption[c] = overrideValue->absorption[c];
            mat.albedo[c] *= overrideValue->tint[c];
        }
    }
    if ( physical.enabled ) {
        mat.physicalMaterial = calibrated ? PT_PHYSICAL_CALIBRATED : PT_PHYSICAL_AUTHORED;
        mat.transmission = physical.transmission;
        mat.ior = physical.ior;
        mat.thinTransmission = physical.thin;
        mat.thickness = physical.thickness;
        for ( int c = 0; c < 3; c++ ) mat.absorption[c] = physical.absorption[c];
        if ( physical.roughness >= 0.0f ) mat.roughness = physical.roughness;
        if ( physical.metallic >= 0.0f ) mat.metallic = physical.metallic;
        // Physical transport replaces legacy overlay compositing, not its texture.
        mat.blendMode = PT_BLEND_OPAQUE;
        if ( mat.isAmbientOnly ) {
            mat.emission[0] = mat.emission[1] = mat.emission[2] = 0.0f;
            mat.emissionTexture = -1;
            emission.Zero();
        }
        mat.isAmbientOnly = 0;
        mat.decalOverlay = mat.emissiveOverlay = 0;
        if ( !calibrated || physical.transmission > 0 ) mat.noShadows = 0;
    }
#endif

    #undef SAFE_REG
}
