
/*
 * Copyright 2012 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *	Tom Stellard <thomas.stellard@amd.com>
 *	Michel Dänzer <michel.daenzer@amd.com>
 *      Christian König <christian.koenig@amd.com>
 */

#include "gallivm/lp_bld_tgsi_action.h"
#include "gallivm/lp_bld_const.h"
#include "gallivm/lp_bld_gather.h"
#include "gallivm/lp_bld_intr.h"
#include "gallivm/lp_bld_logic.h"
#include "gallivm/lp_bld_tgsi.h"
#include "gallivm/lp_bld_arit.h"
#include "gallivm/lp_bld_flow.h"
#include "radeon_llvm.h"
#include "radeon_llvm_emit.h"
#include "util/u_memory.h"
#include "tgsi/tgsi_info.h"
#include "tgsi/tgsi_parse.h"
#include "tgsi/tgsi_scan.h"
#include "tgsi/tgsi_util.h"
#include "tgsi/tgsi_dump.h"

#include "radeonsi_pipe.h"
#include "radeonsi_shader.h"
#include "si_state.h"
#include "sid.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>

struct si_shader_context
{
	struct radeon_llvm_context radeon_bld;
	struct tgsi_parse_context parse;
	struct tgsi_token * tokens;
	struct si_pipe_shader *shader;
	unsigned type; /* TGSI_PROCESSOR_* specifies the type of shader. */
	int param_streamout_config;
	int param_streamout_write_index;
	int param_streamout_offset[4];
	int param_vertex_id;
	int param_instance_id;
	LLVMValueRef const_md;
	LLVMValueRef const_resource[NUM_CONST_BUFFERS];
#if HAVE_LLVM >= 0x0304
	LLVMValueRef ddxy_lds;
#endif
	LLVMValueRef *constants[NUM_CONST_BUFFERS];
	LLVMValueRef *resources;
	LLVMValueRef *samplers;
	LLVMValueRef so_buffers[4];
};

static struct si_shader_context * si_shader_context(
	struct lp_build_tgsi_context * bld_base)
{
	return (struct si_shader_context *)bld_base;
}


#define PERSPECTIVE_BASE 0
#define LINEAR_BASE 9

#define SAMPLE_OFFSET 0
#define CENTER_OFFSET 2
#define CENTROID_OFSET 4

#define USE_SGPR_MAX_SUFFIX_LEN 5
#define CONST_ADDR_SPACE 2
#define LOCAL_ADDR_SPACE 3
#define USER_SGPR_ADDR_SPACE 8

/**
 * Build an LLVM bytecode indexed load using LLVMBuildGEP + LLVMBuildLoad
 *
 * @param offset The offset parameter specifies the number of
 * elements to offset, not the number of bytes or dwords.  An element is the
 * the type pointed to by the base_ptr parameter (e.g. int is the element of
 * an int* pointer)
 *
 * When LLVM lowers the load instruction, it will convert the element offset
 * into a dword offset automatically.
 *
 */
static LLVMValueRef build_indexed_load(
	struct si_shader_context * si_shader_ctx,
	LLVMValueRef base_ptr,
	LLVMValueRef offset)
{
	struct lp_build_context * base = &si_shader_ctx->radeon_bld.soa.bld_base.base;

	LLVMValueRef indices[2] = {
		LLVMConstInt(LLVMInt64TypeInContext(base->gallivm->context), 0, false),
		offset
	};
	LLVMValueRef computed_ptr = LLVMBuildGEP(
		base->gallivm->builder, base_ptr, indices, 2, "");

	LLVMValueRef result = LLVMBuildLoad(base->gallivm->builder, computed_ptr, "");
	LLVMSetMetadata(result, 1, si_shader_ctx->const_md);
	return result;
}

static LLVMValueRef get_instance_index_for_fetch(
	struct radeon_llvm_context * radeon_bld,
	unsigned divisor)
{
	struct si_shader_context *si_shader_ctx =
		si_shader_context(&radeon_bld->soa.bld_base);
	struct gallivm_state * gallivm = radeon_bld->soa.bld_base.base.gallivm;

	LLVMValueRef result = LLVMGetParam(radeon_bld->main_fn,
					   si_shader_ctx->param_instance_id);
	result = LLVMBuildAdd(gallivm->builder, result, LLVMGetParam(
			radeon_bld->main_fn, SI_PARAM_START_INSTANCE), "");

	if (divisor > 1)
		result = LLVMBuildUDiv(gallivm->builder, result,
				lp_build_const_int32(gallivm, divisor), "");

	return result;
}

static void declare_input_vs(
	struct si_shader_context * si_shader_ctx,
	unsigned input_index,
	const struct tgsi_full_declaration *decl)
{
	struct lp_build_context * base = &si_shader_ctx->radeon_bld.soa.bld_base.base;
	unsigned divisor = si_shader_ctx->shader->key.vs.instance_divisors[input_index];

	unsigned chan;

	LLVMValueRef t_list_ptr;
	LLVMValueRef t_offset;
	LLVMValueRef t_list;
	LLVMValueRef attribute_offset;
	LLVMValueRef buffer_index;
	LLVMValueRef args[3];
	LLVMTypeRef vec4_type;
	LLVMValueRef input;

	/* Load the T list */
	t_list_ptr = LLVMGetParam(si_shader_ctx->radeon_bld.main_fn, SI_PARAM_VERTEX_BUFFER);

	t_offset = lp_build_const_int32(base->gallivm, input_index);

	t_list = build_indexed_load(si_shader_ctx, t_list_ptr, t_offset);

	/* Build the attribute offset */
	attribute_offset = lp_build_const_int32(base->gallivm, 0);

	if (divisor) {
		/* Build index from instance ID, start instance and divisor */
		si_shader_ctx->shader->shader.uses_instanceid = true;
		buffer_index = get_instance_index_for_fetch(&si_shader_ctx->radeon_bld, divisor);
	} else {
		/* Load the buffer index, which is always stored in VGPR0
		 * for Vertex Shaders */
		buffer_index = LLVMGetParam(si_shader_ctx->radeon_bld.main_fn,
					    si_shader_ctx->param_vertex_id);
	}

	vec4_type = LLVMVectorType(base->elem_type, 4);
	args[0] = t_list;
	args[1] = attribute_offset;
	args[2] = buffer_index;
	input = build_intrinsic(base->gallivm->builder,
		"llvm.SI.vs.load.input", vec4_type, args, 3,
		LLVMReadNoneAttribute | LLVMNoUnwindAttribute);

	/* Break up the vec4 into individual components */
	for (chan = 0; chan < 4; chan++) {
		LLVMValueRef llvm_chan = lp_build_const_int32(base->gallivm, chan);
		/* XXX: Use a helper function for this.  There is one in
 		 * tgsi_llvm.c. */
		si_shader_ctx->radeon_bld.inputs[radeon_llvm_reg_index_soa(input_index, chan)] =
				LLVMBuildExtractElement(base->gallivm->builder,
				input, llvm_chan, "");
	}
}

static void declare_input_fs(
	struct si_shader_context * si_shader_ctx,
	unsigned input_index,
	const struct tgsi_full_declaration *decl)
{
	struct si_shader *shader = &si_shader_ctx->shader->shader;
	struct lp_build_context * base =
				&si_shader_ctx->radeon_bld.soa.bld_base.base;
	struct lp_build_context *uint =
				&si_shader_ctx->radeon_bld.soa.bld_base.uint_bld;
	struct gallivm_state * gallivm = base->gallivm;
	LLVMTypeRef input_type = LLVMFloatTypeInContext(gallivm->context);
	LLVMValueRef main_fn = si_shader_ctx->radeon_bld.main_fn;

	LLVMValueRef interp_param;
	const char * intr_name;

	/* This value is:
	 * [15:0] NewPrimMask (Bit mask for each quad.  It is set it the
	 *                     quad begins a new primitive.  Bit 0 always needs
	 *                     to be unset)
	 * [32:16] ParamOffset
	 *
	 */
	LLVMValueRef params = LLVMGetParam(si_shader_ctx->radeon_bld.main_fn, SI_PARAM_PRIM_MASK);
	LLVMValueRef attr_number;

	unsigned chan;

	if (decl->Semantic.Name == TGSI_SEMANTIC_POSITION) {
		for (chan = 0; chan < TGSI_NUM_CHANNELS; chan++) {
			unsigned soa_index =
				radeon_llvm_reg_index_soa(input_index, chan);
			si_shader_ctx->radeon_bld.inputs[soa_index] =
				LLVMGetParam(main_fn, SI_PARAM_POS_X_FLOAT + chan);

			if (chan == 3)
				/* RCP for fragcoord.w */
				si_shader_ctx->radeon_bld.inputs[soa_index] =
					LLVMBuildFDiv(gallivm->builder,
						      lp_build_const_float(gallivm, 1.0f),
						      si_shader_ctx->radeon_bld.inputs[soa_index],
						      "");
		}
		return;
	}

	if (decl->Semantic.Name == TGSI_SEMANTIC_FACE) {
		LLVMValueRef face, is_face_positive;

		face = LLVMGetParam(main_fn, SI_PARAM_FRONT_FACE);

		is_face_positive = LLVMBuildFCmp(gallivm->builder,
						 LLVMRealUGT, face,
						 lp_build_const_float(gallivm, 0.0f),
						 "");

		si_shader_ctx->radeon_bld.inputs[radeon_llvm_reg_index_soa(input_index, 0)] =
			LLVMBuildSelect(gallivm->builder,
					is_face_positive,
					lp_build_const_float(gallivm, 1.0f),
					lp_build_const_float(gallivm, 0.0f),
					"");
		si_shader_ctx->radeon_bld.inputs[radeon_llvm_reg_index_soa(input_index, 1)] =
		si_shader_ctx->radeon_bld.inputs[radeon_llvm_reg_index_soa(input_index, 2)] =
			lp_build_const_float(gallivm, 0.0f);
		si_shader_ctx->radeon_bld.inputs[radeon_llvm_reg_index_soa(input_index, 3)] =
			lp_build_const_float(gallivm, 1.0f);

		return;
	}

	shader->input[input_index].param_offset = shader->ninterp++;
	attr_number = lp_build_const_int32(gallivm,
					   shader->input[input_index].param_offset);

	/* XXX: Handle all possible interpolation modes */
	switch (decl->Interp.Interpolate) {
	case TGSI_INTERPOLATE_COLOR:
		if (si_shader_ctx->shader->key.ps.flatshade) {
			interp_param = 0;
		} else {
			if (decl->Interp.Centroid)
				interp_param = LLVMGetParam(main_fn, SI_PARAM_PERSP_CENTROID);
			else
				interp_param = LLVMGetParam(main_fn, SI_PARAM_PERSP_CENTER);
		}
		break;
	case TGSI_INTERPOLATE_CONSTANT:
		interp_param = 0;
		break;
	case TGSI_INTERPOLATE_LINEAR:
		if (decl->Interp.Centroid)
			interp_param = LLVMGetParam(main_fn, SI_PARAM_LINEAR_CENTROID);
		else
			interp_param = LLVMGetParam(main_fn, SI_PARAM_LINEAR_CENTER);
		break;
	case TGSI_INTERPOLATE_PERSPECTIVE:
		if (decl->Interp.Centroid)
			interp_param = LLVMGetParam(main_fn, SI_PARAM_PERSP_CENTROID);
		else
			interp_param = LLVMGetParam(main_fn, SI_PARAM_PERSP_CENTER);
		break;
	default:
		fprintf(stderr, "Warning: Unhandled interpolation mode.\n");
		return;
	}

	intr_name = interp_param ? "llvm.SI.fs.interp" : "llvm.SI.fs.constant";

	/* XXX: Could there be more than TGSI_NUM_CHANNELS (4) ? */
	if (decl->Semantic.Name == TGSI_SEMANTIC_COLOR &&
	    si_shader_ctx->shader->key.ps.color_two_side) {
		LLVMValueRef args[4];
		LLVMValueRef face, is_face_positive;
		LLVMValueRef back_attr_number =
			lp_build_const_int32(gallivm,
					     shader->input[input_index].param_offset + 1);

		face = LLVMGetParam(main_fn, SI_PARAM_FRONT_FACE);

		is_face_positive = LLVMBuildFCmp(gallivm->builder,
						 LLVMRealUGT, face,
						 lp_build_const_float(gallivm, 0.0f),
						 "");

		args[2] = params;
		args[3] = interp_param;
		for (chan = 0; chan < TGSI_NUM_CHANNELS; chan++) {
			LLVMValueRef llvm_chan = lp_build_const_int32(gallivm, chan);
			unsigned soa_index = radeon_llvm_reg_index_soa(input_index, chan);
			LLVMValueRef front, back;

			args[0] = llvm_chan;
			args[1] = attr_number;
			front = build_intrinsic(base->gallivm->builder, intr_name,
						input_type, args, args[3] ? 4 : 3,
						LLVMReadNoneAttribute | LLVMNoUnwindAttribute);

			args[1] = back_attr_number;
			back = build_intrinsic(base->gallivm->builder, intr_name,
					       input_type, args, args[3] ? 4 : 3,
					       LLVMReadNoneAttribute | LLVMNoUnwindAttribute);

			si_shader_ctx->radeon_bld.inputs[soa_index] =
				LLVMBuildSelect(gallivm->builder,
						is_face_positive,
						front,
						back,
						"");
		}

		shader->ninterp++;
	} else if (decl->Semantic.Name == TGSI_SEMANTIC_FOG) {
		LLVMValueRef args[4];

		args[0] = uint->zero;
		args[1] = attr_number;
		args[2] = params;
		args[3] = interp_param;
		si_shader_ctx->radeon_bld.inputs[radeon_llvm_reg_index_soa(input_index, 0)] =
			build_intrinsic(base->gallivm->builder, intr_name,
						input_type, args, args[3] ? 4 : 3,
						LLVMReadNoneAttribute | LLVMNoUnwindAttribute);
		si_shader_ctx->radeon_bld.inputs[radeon_llvm_reg_index_soa(input_index, 1)] =
		si_shader_ctx->radeon_bld.inputs[radeon_llvm_reg_index_soa(input_index, 2)] =
			lp_build_const_float(gallivm, 0.0f);
		si_shader_ctx->radeon_bld.inputs[radeon_llvm_reg_index_soa(input_index, 3)] =
			lp_build_const_float(gallivm, 1.0f);
	} else {
		for (chan = 0; chan < TGSI_NUM_CHANNELS; chan++) {
			LLVMValueRef args[4];
			LLVMValueRef llvm_chan = lp_build_const_int32(gallivm, chan);
			unsigned soa_index = radeon_llvm_reg_index_soa(input_index, chan);
			args[0] = llvm_chan;
			args[1] = attr_number;
			args[2] = params;
			args[3] = interp_param;
			si_shader_ctx->radeon_bld.inputs[soa_index] =
				build_intrinsic(base->gallivm->builder, intr_name,
						input_type, args, args[3] ? 4 : 3,
						LLVMReadNoneAttribute | LLVMNoUnwindAttribute);
		}
	}
}

static void declare_input(
	struct radeon_llvm_context * radeon_bld,
	unsigned input_index,
	const struct tgsi_full_declaration *decl)
{
	struct si_shader_context * si_shader_ctx =
				si_shader_context(&radeon_bld->soa.bld_base);
	if (si_shader_ctx->type == TGSI_PROCESSOR_VERTEX) {
		declare_input_vs(si_shader_ctx, input_index, decl);
	} else if (si_shader_ctx->type == TGSI_PROCESSOR_FRAGMENT) {
		declare_input_fs(si_shader_ctx, input_index, decl);
	} else {
		fprintf(stderr, "Warning: Unsupported shader type,\n");
	}
}

static void declare_system_value(
	struct radeon_llvm_context * radeon_bld,
	unsigned index,
	const struct tgsi_full_declaration *decl)
{
	struct si_shader_context *si_shader_ctx =
		si_shader_context(&radeon_bld->soa.bld_base);
	LLVMValueRef value = 0;

	switch (decl->Semantic.Name) {
	case TGSI_SEMANTIC_INSTANCEID:
		value = LLVMGetParam(radeon_bld->main_fn,
				     si_shader_ctx->param_instance_id);
		break;

	case TGSI_SEMANTIC_VERTEXID:
		value = LLVMGetParam(radeon_bld->main_fn,
				     si_shader_ctx->param_vertex_id);
		break;

	default:
		assert(!"unknown system value");
		return;
	}

	radeon_bld->system_values[index] = value;
}

static LLVMValueRef fetch_constant(
	struct lp_build_tgsi_context * bld_base,
	const struct tgsi_full_src_register *reg,
	enum tgsi_opcode_type type,
	unsigned swizzle)
{
	struct si_shader_context *si_shader_ctx = si_shader_context(bld_base);
	struct lp_build_context * base = &bld_base->base;
	const struct tgsi_ind_register *ireg = &reg->Indirect;
	unsigned buf, idx;

	LLVMValueRef args[2];
	LLVMValueRef addr;
	LLVMValueRef result;

	if (swizzle == LP_CHAN_ALL) {
		unsigned chan;
		LLVMValueRef values[4];
		for (chan = 0; chan < TGSI_NUM_CHANNELS; ++chan)
			values[chan] = fetch_constant(bld_base, reg, type, chan);

		return lp_build_gather_values(bld_base->base.gallivm, values, 4);
	}

	buf = reg->Register.Dimension ? reg->Dimension.Index : 0;
	idx = reg->Register.Index * 4 + swizzle;

	if (!reg->Register.Indirect)
		return bitcast(bld_base, type, si_shader_ctx->constants[buf][idx]);

	args[0] = si_shader_ctx->const_resource[buf];
	args[1] = lp_build_const_int32(base->gallivm, idx * 4);
	addr = si_shader_ctx->radeon_bld.soa.addr[ireg->Index][ireg->Swizzle];
	addr = LLVMBuildLoad(base->gallivm->builder, addr, "load addr reg");
	addr = lp_build_mul_imm(&bld_base->uint_bld, addr, 16);
	args[1] = lp_build_add(&bld_base->uint_bld, addr, args[1]);

	result = build_intrinsic(base->gallivm->builder, "llvm.SI.load.const", base->elem_type,
                                 args, 2, LLVMReadNoneAttribute | LLVMNoUnwindAttribute);

	return bitcast(bld_base, type, result);
}

/* Initialize arguments for the shader export intrinsic */
static void si_llvm_init_export_args(struct lp_build_tgsi_context *bld_base,
				     struct tgsi_full_declaration *d,
				     unsigned index,
				     unsigned target,
				     LLVMValueRef *args)
{
	struct si_shader_context *si_shader_ctx = si_shader_context(bld_base);
	struct lp_build_context *uint =
				&si_shader_ctx->radeon_bld.soa.bld_base.uint_bld;
	struct lp_build_context *base = &bld_base->base;
	unsigned compressed = 0;
	unsigned chan;

	if (si_shader_ctx->type == TGSI_PROCESSOR_FRAGMENT) {
		int cbuf = target - V_008DFC_SQ_EXP_MRT;

		if (cbuf >= 0 && cbuf < 8) {
			compressed = (si_shader_ctx->shader->key.ps.export_16bpc >> cbuf) & 0x1;

			if (compressed)
				si_shader_ctx->shader->spi_shader_col_format |=
					V_028714_SPI_SHADER_FP16_ABGR << (4 * cbuf);
			else
				si_shader_ctx->shader->spi_shader_col_format |=
					V_028714_SPI_SHADER_32_ABGR << (4 * cbuf);

			si_shader_ctx->shader->cb_shader_mask |= 0xf << (4 * cbuf);
		}
	}

	if (compressed) {
		/* Pixel shader needs to pack output values before export */
		for (chan = 0; chan < 2; chan++ ) {
			LLVMValueRef *out_ptr =
				si_shader_ctx->radeon_bld.soa.outputs[index];
			args[0] = LLVMBuildLoad(base->gallivm->builder,
						out_ptr[2 * chan], "");
			args[1] = LLVMBuildLoad(base->gallivm->builder,
						out_ptr[2 * chan + 1], "");
			args[chan + 5] =
				build_intrinsic(base->gallivm->builder,
						"llvm.SI.packf16",
						LLVMInt32TypeInContext(base->gallivm->context),
						args, 2,
						LLVMReadNoneAttribute | LLVMNoUnwindAttribute);
			args[chan + 7] = args[chan + 5] =
				LLVMBuildBitCast(base->gallivm->builder,
						 args[chan + 5],
						 LLVMFloatTypeInContext(base->gallivm->context),
						 "");
		}

		/* Set COMPR flag */
		args[4] = uint->one;
	} else {
		for (chan = 0; chan < 4; chan++ ) {
			LLVMValueRef out_ptr =
				si_shader_ctx->radeon_bld.soa.outputs[index][chan];
			/* +5 because the first output value will be
			 * the 6th argument to the intrinsic. */
			args[chan + 5] = LLVMBuildLoad(base->gallivm->builder,
						       out_ptr, "");
		}

		/* Clear COMPR flag */
		args[4] = uint->zero;
	}

	/* XXX: This controls which components of the output
	 * registers actually get exported. (e.g bit 0 means export
	 * X component, bit 1 means export Y component, etc.)  I'm
	 * hard coding this to 0xf for now.  In the future, we might
	 * want to do something else. */
	args[0] = lp_build_const_int32(base->gallivm, 0xf);

	/* Specify whether the EXEC mask represents the valid mask */
	args[1] = uint->zero;

	/* Specify whether this is the last export */
	args[2] = uint->zero;

	/* Specify the target we are exporting */
	args[3] = lp_build_const_int32(base->gallivm, target);

	/* XXX: We probably need to keep track of the output
	 * values, so we know what we are passing to the next
	 * stage. */
}

static void si_alpha_test(struct lp_build_tgsi_context *bld_base,
			  unsigned index)
{
	struct si_shader_context *si_shader_ctx = si_shader_context(bld_base);
	struct gallivm_state *gallivm = bld_base->base.gallivm;

	if (si_shader_ctx->shader->key.ps.alpha_func != PIPE_FUNC_NEVER) {
		LLVMValueRef out_ptr = si_shader_ctx->radeon_bld.soa.outputs[index][3];
		LLVMValueRef alpha_ref = LLVMGetParam(si_shader_ctx->radeon_bld.main_fn,
				SI_PARAM_ALPHA_REF);

		LLVMValueRef alpha_pass =
			lp_build_cmp(&bld_base->base,
				     si_shader_ctx->shader->key.ps.alpha_func,
				     LLVMBuildLoad(gallivm->builder, out_ptr, ""),
				     alpha_ref);
		LLVMValueRef arg =
			lp_build_select(&bld_base->base,
					alpha_pass,
					lp_build_const_float(gallivm, 1.0f),
					lp_build_const_float(gallivm, -1.0f));

		build_intrinsic(gallivm->builder,
				"llvm.AMDGPU.kill",
				LLVMVoidTypeInContext(gallivm->context),
				&arg, 1, 0);
	} else {
		build_intrinsic(gallivm->builder,
				"llvm.AMDGPU.kilp",
				LLVMVoidTypeInContext(gallivm->context),
				NULL, 0, 0);
	}
}

static void si_alpha_to_one(struct lp_build_tgsi_context *bld_base,
			    unsigned index)
{
	struct si_shader_context *si_shader_ctx = si_shader_context(bld_base);

	/* set alpha to one */
	LLVMBuildStore(bld_base->base.gallivm->builder,
		       bld_base->base.one,
		       si_shader_ctx->radeon_bld.soa.outputs[index][3]);
}

static void si_llvm_emit_clipvertex(struct lp_build_tgsi_context * bld_base,
				    LLVMValueRef (*pos)[9], unsigned index)
{
	struct si_shader_context *si_shader_ctx = si_shader_context(bld_base);
	struct si_pipe_shader *shader = si_shader_ctx->shader;
	struct lp_build_context *base = &bld_base->base;
	struct lp_build_context *uint = &si_shader_ctx->radeon_bld.soa.bld_base.uint_bld;
	unsigned reg_index;
	unsigned chan;
	unsigned const_chan;
	LLVMValueRef out_elts[4];
	LLVMValueRef base_elt;
	LLVMValueRef ptr = LLVMGetParam(si_shader_ctx->radeon_bld.main_fn, SI_PARAM_CONST);
	LLVMValueRef constbuf_index = lp_build_const_int32(base->gallivm, NUM_PIPE_CONST_BUFFERS);
	LLVMValueRef const_resource = build_indexed_load(si_shader_ctx, ptr, constbuf_index);

	for (chan = 0; chan < TGSI_NUM_CHANNELS; chan++) {
		LLVMValueRef out_ptr = si_shader_ctx->radeon_bld.soa.outputs[index][chan];
		out_elts[chan] = LLVMBuildLoad(base->gallivm->builder, out_ptr, "");
	}

	for (reg_index = 0; reg_index < 2; reg_index ++) {
		LLVMValueRef *args = pos[2 + reg_index];

		if (!(shader->key.vs.ucps_enabled & (1 << reg_index)))
			continue;

		shader->shader.clip_dist_write |= 0xf << (4 * reg_index);

		args[5] =
		args[6] =
		args[7] =
		args[8] = lp_build_const_float(base->gallivm, 0.0f);

		/* Compute dot products of position and user clip plane vectors */
		for (chan = 0; chan < TGSI_NUM_CHANNELS; chan++) {
			for (const_chan = 0; const_chan < TGSI_NUM_CHANNELS; const_chan++) {
				args[0] = const_resource;
				args[1] = lp_build_const_int32(base->gallivm,
							       ((reg_index * 4 + chan) * 4 +
								const_chan) * 4);
				base_elt = build_intrinsic(base->gallivm->builder,
							   "llvm.SI.load.const",
							   base->elem_type,
							   args, 2,
							   LLVMReadNoneAttribute | LLVMNoUnwindAttribute);
				args[5 + chan] =
					lp_build_add(base, args[5 + chan],
						     lp_build_mul(base, base_elt,
								  out_elts[const_chan]));
			}
		}

		args[0] = lp_build_const_int32(base->gallivm, 0xf);
		args[1] = uint->zero;
		args[2] = uint->zero;
		args[3] = lp_build_const_int32(base->gallivm,
					       V_008DFC_SQ_EXP_POS + 2 + reg_index);
		args[4] = uint->zero;
	}
}

static void si_dump_streamout(struct pipe_stream_output_info *so)
{
	unsigned i;

	if (so->num_outputs)
		fprintf(stderr, "STREAMOUT\n");

	for (i = 0; i < so->num_outputs; i++) {
		unsigned mask = ((1 << so->output[i].num_components) - 1) <<
				so->output[i].start_component;
		fprintf(stderr, "  %i: BUF%i[%i..%i] <- OUT[%i].%s%s%s%s\n",
			i, so->output[i].output_buffer,
			so->output[i].dst_offset, so->output[i].dst_offset + so->output[i].num_components - 1,
			so->output[i].register_index,
			mask & 1 ? "x" : "",
		        mask & 2 ? "y" : "",
		        mask & 4 ? "z" : "",
		        mask & 8 ? "w" : "");
	}
}

/* TBUFFER_STORE_FORMAT_{X,XY,XYZ,XYZW} <- the suffix is selected by num_channels=1..4.
 * The type of vdata must be one of i32 (num_channels=1), v2i32 (num_channels=2),
 * or v4i32 (num_channels=3,4). */
static void build_tbuffer_store(struct si_shader_context *shader,
				LLVMValueRef rsrc,
				LLVMValueRef vdata,
				unsigned num_channels,
				LLVMValueRef vaddr,
				LLVMValueRef soffset,
				unsigned inst_offset,
				unsigned dfmt,
				unsigned nfmt,
				unsigned offen,
				unsigned idxen,
				unsigned glc,
				unsigned slc,
				unsigned tfe)
{
	struct gallivm_state *gallivm = &shader->radeon_bld.gallivm;
	LLVMTypeRef i32 = LLVMInt32TypeInContext(gallivm->context);
	LLVMValueRef args[] = {
		rsrc,
		vdata,
		LLVMConstInt(i32, num_channels, 0),
		vaddr,
		soffset,
		LLVMConstInt(i32, inst_offset, 0),
		LLVMConstInt(i32, dfmt, 0),
		LLVMConstInt(i32, nfmt, 0),
		LLVMConstInt(i32, offen, 0),
		LLVMConstInt(i32, idxen, 0),
		LLVMConstInt(i32, glc, 0),
		LLVMConstInt(i32, slc, 0),
		LLVMConstInt(i32, tfe, 0)
	};

	/* The intrinsic is overloaded, we need to add a type suffix for overloading to work. */
	unsigned func = CLAMP(num_channels, 1, 3) - 1;
	const char *types[] = {"i32", "v2i32", "v4i32"};
	char name[256];
	snprintf(name, sizeof(name), "llvm.SI.tbuffer.store.%s", types[func]);

	lp_build_intrinsic(gallivm->builder, name,
			   LLVMVoidTypeInContext(gallivm->context),
			   args, Elements(args));
}

static void build_streamout_store(struct si_shader_context *shader,
				  LLVMValueRef rsrc,
				  LLVMValueRef vdata,
				  unsigned num_channels,
				  LLVMValueRef vaddr,
				  LLVMValueRef soffset,
				  unsigned inst_offset)
{
	static unsigned dfmt[] = {
		V_008F0C_BUF_DATA_FORMAT_32,
		V_008F0C_BUF_DATA_FORMAT_32_32,
		V_008F0C_BUF_DATA_FORMAT_32_32_32,
		V_008F0C_BUF_DATA_FORMAT_32_32_32_32
	};
	assert(num_channels >= 1 && num_channels <= 4);

	build_tbuffer_store(shader, rsrc, vdata, num_channels, vaddr, soffset,
			    inst_offset, dfmt[num_channels-1],
			    V_008F0C_BUF_NUM_FORMAT_UINT, 1, 0, 1, 1, 0);
}

/* On SI, the vertex shader is responsible for writing streamout data
 * to buffers. */
static void si_llvm_emit_streamout(struct si_shader_context *shader)
{
	struct pipe_stream_output_info *so = &shader->shader->selector->so;
	struct gallivm_state *gallivm = &shader->radeon_bld.gallivm;
	LLVMBuilderRef builder = gallivm->builder;
	int i, j;
	struct lp_build_if_state if_ctx;

	LLVMTypeRef i32 = LLVMInt32TypeInContext(gallivm->context);

	LLVMValueRef so_param =
		LLVMGetParam(shader->radeon_bld.main_fn,
			     shader->param_streamout_config);

	/* Get bits [22:16], i.e. (so_param >> 16) & 127; */
	LLVMValueRef so_vtx_count =
		LLVMBuildAnd(builder,
			     LLVMBuildLShr(builder, so_param,
					   LLVMConstInt(i32, 16, 0), ""),
			     LLVMConstInt(i32, 127, 0), "");

	LLVMValueRef tid = build_intrinsic(builder, "llvm.SI.tid", i32,
					   NULL, 0, LLVMReadNoneAttribute);

	/* can_emit = tid < so_vtx_count; */
	LLVMValueRef can_emit =
		LLVMBuildICmp(builder, LLVMIntULT, tid, so_vtx_count, "");

	/* Emit the streamout code conditionally. This actually avoids
	 * out-of-bounds buffer access. The hw tells us via the SGPR
	 * (so_vtx_count) which threads are allowed to emit streamout data. */
	lp_build_if(&if_ctx, gallivm, can_emit);
	{
		/* The buffer offset is computed as follows:
		 *   ByteOffset = streamout_offset[buffer_id]*4 +
		 *                (streamout_write_index + thread_id)*stride[buffer_id] +
		 *                attrib_offset
                 */

		LLVMValueRef so_write_index =
			LLVMGetParam(shader->radeon_bld.main_fn,
				     shader->param_streamout_write_index);

		/* Compute (streamout_write_index + thread_id). */
		so_write_index = LLVMBuildAdd(builder, so_write_index, tid, "");

		/* Compute the write offset for each enabled buffer. */
		LLVMValueRef so_write_offset[4] = {};
		for (i = 0; i < 4; i++) {
			if (!so->stride[i])
				continue;

			LLVMValueRef so_offset = LLVMGetParam(shader->radeon_bld.main_fn,
							      shader->param_streamout_offset[i]);
			so_offset = LLVMBuildMul(builder, so_offset, LLVMConstInt(i32, 4, 0), "");

			so_write_offset[i] = LLVMBuildMul(builder, so_write_index,
							  LLVMConstInt(i32, so->stride[i]*4, 0), "");
			so_write_offset[i] = LLVMBuildAdd(builder, so_write_offset[i], so_offset, "");
		}

		LLVMValueRef (*outputs)[TGSI_NUM_CHANNELS] = shader->radeon_bld.soa.outputs;

		/* Write streamout data. */
		for (i = 0; i < so->num_outputs; i++) {
			unsigned buf_idx = so->output[i].output_buffer;
			unsigned reg = so->output[i].register_index;
			unsigned start = so->output[i].start_component;
			unsigned num_comps = so->output[i].num_components;
			LLVMValueRef out[4];

			assert(num_comps && num_comps <= 4);
			if (!num_comps || num_comps > 4)
				continue;

			/* Load the output as int. */
			for (j = 0; j < num_comps; j++) {
				out[j] = LLVMBuildLoad(builder, outputs[reg][start+j], "");
				out[j] = LLVMBuildBitCast(builder, out[j], i32, "");
			}

			/* Pack the output. */
			LLVMValueRef vdata = NULL;

			switch (num_comps) {
			case 1: /* as i32 */
				vdata = out[0];
				break;
			case 2: /* as v2i32 */
			case 3: /* as v4i32 (aligned to 4) */
			case 4: /* as v4i32 */
				vdata = LLVMGetUndef(LLVMVectorType(i32, util_next_power_of_two(num_comps)));
				for (j = 0; j < num_comps; j++) {
					vdata = LLVMBuildInsertElement(builder, vdata, out[j],
								       LLVMConstInt(i32, j, 0), "");
				}
				break;
			}

			build_streamout_store(shader, shader->so_buffers[buf_idx],
					      vdata, num_comps,
					      so_write_offset[buf_idx],
					      LLVMConstInt(i32, 0, 0),
					      so->output[i].dst_offset*4);
		}
	}
	lp_build_endif(&if_ctx);
}

/* XXX: This is partially implemented for VS only at this point.  It is not complete */
static void si_llvm_emit_epilogue(struct lp_build_tgsi_context * bld_base)
{
	struct si_shader_context * si_shader_ctx = si_shader_context(bld_base);
	struct si_shader * shader = &si_shader_ctx->shader->shader;
	struct lp_build_context * base = &bld_base->base;
	struct lp_build_context * uint =
				&si_shader_ctx->radeon_bld.soa.bld_base.uint_bld;
	struct tgsi_parse_context *parse = &si_shader_ctx->parse;
	LLVMValueRef args[9];
	LLVMValueRef last_args[9] = { 0 };
	LLVMValueRef pos_args[4][9] = { { 0 } };
	unsigned semantic_name;
	unsigned param_count = 0;
	int depth_index = -1, stencil_index = -1;
	int i;

	if (si_shader_ctx->shader->selector->so.num_outputs) {
		si_llvm_emit_streamout(si_shader_ctx);
	}

	while (!tgsi_parse_end_of_tokens(parse)) {
		struct tgsi_full_declaration *d =
					&parse->FullToken.FullDeclaration;
		unsigned target;
		unsigned index;

		tgsi_parse_token(parse);

		if (parse->FullToken.Token.Type == TGSI_TOKEN_TYPE_PROPERTY &&
		    parse->FullToken.FullProperty.Property.PropertyName ==
		    TGSI_PROPERTY_FS_COLOR0_WRITES_ALL_CBUFS)
			shader->fs_write_all = TRUE;

		if (parse->FullToken.Token.Type != TGSI_TOKEN_TYPE_DECLARATION)
			continue;

		switch (d->Declaration.File) {
		case TGSI_FILE_INPUT:
			i = shader->ninput++;
			assert(i < Elements(shader->input));
			shader->input[i].name = d->Semantic.Name;
			shader->input[i].sid = d->Semantic.Index;
			shader->input[i].interpolate = d->Interp.Interpolate;
			shader->input[i].centroid = d->Interp.Centroid;
			continue;

		case TGSI_FILE_OUTPUT:
			i = shader->noutput++;
			assert(i < Elements(shader->output));
			shader->output[i].name = d->Semantic.Name;
			shader->output[i].sid = d->Semantic.Index;
			shader->output[i].interpolate = d->Interp.Interpolate;
			break;

		default:
			continue;
		}

		semantic_name = d->Semantic.Name;
handle_semantic:
		for (index = d->Range.First; index <= d->Range.Last; index++) {
			/* Select the correct target */
			switch(semantic_name) {
			case TGSI_SEMANTIC_PSIZE:
				shader->vs_out_misc_write = 1;
				shader->vs_out_point_size = 1;
				target = V_008DFC_SQ_EXP_POS + 1;
				break;
			case TGSI_SEMANTIC_POSITION:
				if (si_shader_ctx->type == TGSI_PROCESSOR_VERTEX) {
					target = V_008DFC_SQ_EXP_POS;
					break;
				} else {
					depth_index = index;
					continue;
				}
			case TGSI_SEMANTIC_STENCIL:
				stencil_index = index;
				continue;
			case TGSI_SEMANTIC_COLOR:
				if (si_shader_ctx->type == TGSI_PROCESSOR_VERTEX) {
			case TGSI_SEMANTIC_BCOLOR:
					target = V_008DFC_SQ_EXP_PARAM + param_count;
					shader->output[i].param_offset = param_count;
					param_count++;
				} else {
					target = V_008DFC_SQ_EXP_MRT + shader->output[i].sid;
					if (si_shader_ctx->shader->key.ps.alpha_to_one) {
						si_alpha_to_one(bld_base, index);
					}
					if (shader->output[i].sid == 0 &&
					    si_shader_ctx->shader->key.ps.alpha_func != PIPE_FUNC_ALWAYS)
						si_alpha_test(bld_base, index);
				}
				break;
			case TGSI_SEMANTIC_CLIPDIST:
				if (!(si_shader_ctx->shader->key.vs.ucps_enabled &
				      (1 << d->Semantic.Index)))
					continue;
				shader->clip_dist_write |=
					d->Declaration.UsageMask << (d->Semantic.Index << 2);
				target = V_008DFC_SQ_EXP_POS + 2 + d->Semantic.Index;
				break;
			case TGSI_SEMANTIC_CLIPVERTEX:
				si_llvm_emit_clipvertex(bld_base, pos_args, index);
				continue;
			case TGSI_SEMANTIC_FOG:
			case TGSI_SEMANTIC_GENERIC:
				target = V_008DFC_SQ_EXP_PARAM + param_count;
				shader->output[i].param_offset = param_count;
				param_count++;
				break;
			default:
				target = 0;
				fprintf(stderr,
					"Warning: SI unhandled output type:%d\n",
					semantic_name);
			}

			si_llvm_init_export_args(bld_base, d, index, target, args);

			if (si_shader_ctx->type == TGSI_PROCESSOR_VERTEX &&
			    target >= V_008DFC_SQ_EXP_POS &&
			    target <= (V_008DFC_SQ_EXP_POS + 3)) {
				memcpy(pos_args[target - V_008DFC_SQ_EXP_POS],
				       args, sizeof(args));
			} else if (si_shader_ctx->type == TGSI_PROCESSOR_FRAGMENT &&
				   semantic_name == TGSI_SEMANTIC_COLOR) {
				if (last_args[0]) {
					lp_build_intrinsic(base->gallivm->builder,
							   "llvm.SI.export",
							   LLVMVoidTypeInContext(base->gallivm->context),
							   last_args, 9);
				}

				memcpy(last_args, args, sizeof(args));
			} else {
				lp_build_intrinsic(base->gallivm->builder,
						   "llvm.SI.export",
						   LLVMVoidTypeInContext(base->gallivm->context),
						   args, 9);
			}

		}

		if (semantic_name == TGSI_SEMANTIC_CLIPDIST) {
			semantic_name = TGSI_SEMANTIC_GENERIC;
			goto handle_semantic;
		}
	}

	if (depth_index >= 0 || stencil_index >= 0) {
		LLVMValueRef out_ptr;
		unsigned mask = 0;

		/* Specify the target we are exporting */
		args[3] = lp_build_const_int32(base->gallivm, V_008DFC_SQ_EXP_MRTZ);

		if (depth_index >= 0) {
			out_ptr = si_shader_ctx->radeon_bld.soa.outputs[depth_index][2];
			args[5] = LLVMBuildLoad(base->gallivm->builder, out_ptr, "");
			mask |= 0x1;

			if (stencil_index < 0) {
				args[6] =
				args[7] =
				args[8] = args[5];
			}
		}

		if (stencil_index >= 0) {
			out_ptr = si_shader_ctx->radeon_bld.soa.outputs[stencil_index][1];
			args[7] =
			args[8] =
			args[6] = LLVMBuildLoad(base->gallivm->builder, out_ptr, "");
			/* Only setting the stencil component bit (0x2) here
			 * breaks some stencil piglit tests
			 */
			mask |= 0x3;

			if (depth_index < 0)
				args[5] = args[6];
		}

		/* Specify which components to enable */
		args[0] = lp_build_const_int32(base->gallivm, mask);

		args[1] =
		args[2] =
		args[4] = uint->zero;

		if (last_args[0])
			lp_build_intrinsic(base->gallivm->builder,
					   "llvm.SI.export",
					   LLVMVoidTypeInContext(base->gallivm->context),
					   args, 9);
		else
			memcpy(last_args, args, sizeof(args));
	}

	if (si_shader_ctx->type == TGSI_PROCESSOR_VERTEX) {
		unsigned pos_idx = 0;

		/* We need to add the position output manually if it's missing. */
		if (!pos_args[0][0]) {
			pos_args[0][0] = lp_build_const_int32(base->gallivm, 0xf); /* writemask */
			pos_args[0][1] = uint->zero; /* EXEC mask */
			pos_args[0][2] = uint->zero; /* last export? */
			pos_args[0][3] = lp_build_const_int32(base->gallivm, V_008DFC_SQ_EXP_POS);
			pos_args[0][4] = uint->zero; /* COMPR flag */
			pos_args[0][5] = base->zero; /* X */
			pos_args[0][6] = base->zero; /* Y */
			pos_args[0][7] = base->zero; /* Z */
			pos_args[0][8] = base->one;  /* W */
		}

		for (i = 0; i < 4; i++)
			if (pos_args[i][0])
				shader->nr_pos_exports++;

		for (i = 0; i < 4; i++) {
			if (!pos_args[i][0])
				continue;

			/* Specify the target we are exporting */
			pos_args[i][3] = lp_build_const_int32(base->gallivm, V_008DFC_SQ_EXP_POS + pos_idx++);

			if (pos_idx == shader->nr_pos_exports)
				/* Specify that this is the last export */
				pos_args[i][2] = uint->one;

			lp_build_intrinsic(base->gallivm->builder,
					   "llvm.SI.export",
					   LLVMVoidTypeInContext(base->gallivm->context),
					   pos_args[i], 9);
		}
	} else {
		if (!last_args[0]) {
			/* Specify which components to enable */
			last_args[0] = lp_build_const_int32(base->gallivm, 0x0);

			/* Specify the target we are exporting */
			last_args[3] = lp_build_const_int32(base->gallivm, V_008DFC_SQ_EXP_MRT);

			/* Set COMPR flag to zero to export data as 32-bit */
			last_args[4] = uint->zero;

			/* dummy bits */
			last_args[5]= uint->zero;
			last_args[6]= uint->zero;
			last_args[7]= uint->zero;
			last_args[8]= uint->zero;

			si_shader_ctx->shader->spi_shader_col_format |=
				V_028714_SPI_SHADER_32_ABGR;
			si_shader_ctx->shader->cb_shader_mask |= S_02823C_OUTPUT0_ENABLE(0xf);
		}

		/* Specify whether the EXEC mask represents the valid mask */
		last_args[1] = uint->one;

		if (shader->fs_write_all && shader->nr_cbufs > 1) {
			int i;

			/* Specify that this is not yet the last export */
			last_args[2] = lp_build_const_int32(base->gallivm, 0);

			for (i = 1; i < shader->nr_cbufs; i++) {
				/* Specify the target we are exporting */
				last_args[3] = lp_build_const_int32(base->gallivm,
								    V_008DFC_SQ_EXP_MRT + i);

				lp_build_intrinsic(base->gallivm->builder,
						   "llvm.SI.export",
						   LLVMVoidTypeInContext(base->gallivm->context),
						   last_args, 9);

				si_shader_ctx->shader->spi_shader_col_format |=
					si_shader_ctx->shader->spi_shader_col_format << 4;
				si_shader_ctx->shader->cb_shader_mask |=
					si_shader_ctx->shader->cb_shader_mask << 4;
			}

			last_args[3] = lp_build_const_int32(base->gallivm, V_008DFC_SQ_EXP_MRT);
		}

		/* Specify that this is the last export */
		last_args[2] = lp_build_const_int32(base->gallivm, 1);

		lp_build_intrinsic(base->gallivm->builder,
				   "llvm.SI.export",
				   LLVMVoidTypeInContext(base->gallivm->context),
				   last_args, 9);
	}
/* XXX: Look up what this function does */
/*		ctx->shader->output[i].spi_sid = r600_spi_sid(&ctx->shader->output[i]);*/
}

static const struct lp_build_tgsi_action txf_action;

static void build_tex_intrinsic(const struct lp_build_tgsi_action * action,
				struct lp_build_tgsi_context * bld_base,
				struct lp_build_emit_data * emit_data);

static void tex_fetch_args(
	struct lp_build_tgsi_context * bld_base,
	struct lp_build_emit_data * emit_data)
{
	struct si_shader_context *si_shader_ctx = si_shader_context(bld_base);
	struct gallivm_state *gallivm = bld_base->base.gallivm;
	const struct tgsi_full_instruction * inst = emit_data->inst;
	unsigned opcode = inst->Instruction.Opcode;
	unsigned target = inst->Texture.Texture;
	LLVMValueRef coords[4];
	LLVMValueRef address[16];
	int ref_pos;
	unsigned num_coords = tgsi_util_get_texture_coord_dim(target, &ref_pos);
	unsigned count = 0;
	unsigned chan;
	unsigned sampler_src = emit_data->inst->Instruction.NumSrcRegs - 1;
	unsigned sampler_index = emit_data->inst->Src[sampler_src].Register.Index;

	if (target == TGSI_TEXTURE_BUFFER) {
		LLVMTypeRef i128 = LLVMIntTypeInContext(gallivm->context, 128);
		LLVMTypeRef v2i128 = LLVMVectorType(i128, 2);
		LLVMTypeRef i8 = LLVMInt8TypeInContext(gallivm->context);
		LLVMTypeRef v16i8 = LLVMVectorType(i8, 16);

		/* Truncate v32i8 to v16i8. */
		LLVMValueRef res = si_shader_ctx->resources[sampler_index];
		res = LLVMBuildBitCast(gallivm->builder, res, v2i128, "");
		res = LLVMBuildExtractElement(gallivm->builder, res, bld_base->uint_bld.zero, "");
		res = LLVMBuildBitCast(gallivm->builder, res, v16i8, "");

		emit_data->dst_type = LLVMVectorType(bld_base->base.elem_type, 4);
		emit_data->args[0] = res;
		emit_data->args[1] = bld_base->uint_bld.zero;
		emit_data->args[2] = lp_build_emit_fetch(bld_base, emit_data->inst, 0, 0);
		emit_data->arg_count = 3;
		return;
	}

	/* Fetch and project texture coordinates */
	coords[3] = lp_build_emit_fetch(bld_base, emit_data->inst, 0, TGSI_CHAN_W);
	for (chan = 0; chan < 3; chan++ ) {
		coords[chan] = lp_build_emit_fetch(bld_base,
						   emit_data->inst, 0,
						   chan);
		if (opcode == TGSI_OPCODE_TXP)
			coords[chan] = lp_build_emit_llvm_binary(bld_base,
								 TGSI_OPCODE_DIV,
								 coords[chan],
								 coords[3]);
	}

	if (opcode == TGSI_OPCODE_TXP)
		coords[3] = bld_base->base.one;

	/* Pack LOD bias value */
	if (opcode == TGSI_OPCODE_TXB)
		address[count++] = coords[3];

	if (target == TGSI_TEXTURE_CUBE || target == TGSI_TEXTURE_SHADOWCUBE)
		radeon_llvm_emit_prepare_cube_coords(bld_base, emit_data, coords);

	/* Pack depth comparison value */
	switch (target) {
	case TGSI_TEXTURE_SHADOW1D:
	case TGSI_TEXTURE_SHADOW1D_ARRAY:
	case TGSI_TEXTURE_SHADOW2D:
	case TGSI_TEXTURE_SHADOWRECT:
	case TGSI_TEXTURE_SHADOWCUBE:
	case TGSI_TEXTURE_SHADOW2D_ARRAY:
		assert(ref_pos >= 0);
		address[count++] = coords[ref_pos];
		break;
	case TGSI_TEXTURE_SHADOWCUBE_ARRAY:
		address[count++] = lp_build_emit_fetch(bld_base, inst, 1, 0);
	}

	/* Pack user derivatives */
	if (opcode == TGSI_OPCODE_TXD) {
		for (chan = 0; chan < 2; chan++) {
			address[count++] = lp_build_emit_fetch(bld_base, inst, 1, chan);
			if (num_coords > 1)
				address[count++] = lp_build_emit_fetch(bld_base, inst, 2, chan);
		}
	}

	/* Pack texture coordinates */
	address[count++] = coords[0];
	if (num_coords > 1)
		address[count++] = coords[1];
	if (num_coords > 2)
		address[count++] = coords[2];

	/* Pack LOD or sample index */
	if (opcode == TGSI_OPCODE_TXL || opcode == TGSI_OPCODE_TXF)
		address[count++] = coords[3];

	if (count > 16) {
		assert(!"Cannot handle more than 16 texture address parameters");
		count = 16;
	}

	for (chan = 0; chan < count; chan++ ) {
		address[chan] = LLVMBuildBitCast(gallivm->builder,
						 address[chan],
						 LLVMInt32TypeInContext(gallivm->context),
						 "");
	}

	/* Adjust the sample index according to FMASK.
	 *
	 * For uncompressed MSAA surfaces, FMASK should return 0x76543210,
	 * which is the identity mapping. Each nibble says which physical sample
	 * should be fetched to get that sample.
	 *
	 * For example, 0x11111100 means there are only 2 samples stored and
	 * the second sample covers 3/4 of the pixel. When reading samples 0
	 * and 1, return physical sample 0 (determined by the first two 0s
	 * in FMASK), otherwise return physical sample 1.
	 *
	 * The sample index should be adjusted as follows:
	 *   sample_index = (fmask >> (sample_index * 4)) & 0xF;
	 */
	if (target == TGSI_TEXTURE_2D_MSAA ||
	    target == TGSI_TEXTURE_2D_ARRAY_MSAA) {
		struct lp_build_context *uint_bld = &bld_base->uint_bld;
		struct lp_build_emit_data txf_emit_data = *emit_data;
		LLVMValueRef txf_address[4];
		unsigned txf_count = count;

		memcpy(txf_address, address, sizeof(txf_address));

		if (target == TGSI_TEXTURE_2D_MSAA) {
			txf_address[2] = bld_base->uint_bld.zero;
		}
		txf_address[3] = bld_base->uint_bld.zero;

		/* Pad to a power-of-two size. */
		while (txf_count < util_next_power_of_two(txf_count))
			txf_address[txf_count++] = LLVMGetUndef(LLVMInt32TypeInContext(gallivm->context));

		/* Read FMASK using TXF. */
		txf_emit_data.chan = 0;
		txf_emit_data.dst_type = LLVMVectorType(
			LLVMInt32TypeInContext(bld_base->base.gallivm->context), 4);
		txf_emit_data.args[0] = lp_build_gather_values(gallivm, txf_address, txf_count);
		txf_emit_data.args[1] = si_shader_ctx->resources[FMASK_TEX_OFFSET + sampler_index];
		txf_emit_data.args[2] = lp_build_const_int32(bld_base->base.gallivm,
			target == TGSI_TEXTURE_2D_MSAA ? TGSI_TEXTURE_2D : TGSI_TEXTURE_2D_ARRAY);
		txf_emit_data.arg_count = 3;

		build_tex_intrinsic(&txf_action, bld_base, &txf_emit_data);

		/* Initialize some constants. */
		LLVMValueRef four = LLVMConstInt(uint_bld->elem_type, 4, 0);
		LLVMValueRef F = LLVMConstInt(uint_bld->elem_type, 0xF, 0);

		/* Apply the formula. */
		LLVMValueRef fmask =
			LLVMBuildExtractElement(gallivm->builder,
						txf_emit_data.output[0],
						uint_bld->zero, "");

		unsigned sample_chan = target == TGSI_TEXTURE_2D_MSAA ? 2 : 3;

		LLVMValueRef sample_index4 =
			LLVMBuildMul(gallivm->builder, address[sample_chan], four, "");

		LLVMValueRef shifted_fmask =
			LLVMBuildLShr(gallivm->builder, fmask, sample_index4, "");

		LLVMValueRef final_sample =
			LLVMBuildAnd(gallivm->builder, shifted_fmask, F, "");

		/* Don't rewrite the sample index if WORD1.DATA_FORMAT of the FMASK
		 * resource descriptor is 0 (invalid),
		 */
		LLVMValueRef fmask_desc =
			LLVMBuildBitCast(gallivm->builder,
					 si_shader_ctx->resources[FMASK_TEX_OFFSET + sampler_index],
					 LLVMVectorType(uint_bld->elem_type, 8), "");

		LLVMValueRef fmask_word1 =
			LLVMBuildExtractElement(gallivm->builder, fmask_desc,
						uint_bld->one, "");

		LLVMValueRef word1_is_nonzero =
			LLVMBuildICmp(gallivm->builder, LLVMIntNE,
				      fmask_word1, uint_bld->zero, "");

		/* Replace the MSAA sample index. */
		address[sample_chan] =
			LLVMBuildSelect(gallivm->builder, word1_is_nonzero,
					final_sample, address[sample_chan], "");
	}

	/* Resource */
	emit_data->args[1] = si_shader_ctx->resources[sampler_index];

	if (opcode == TGSI_OPCODE_TXF) {
		/* add tex offsets */
		if (inst->Texture.NumOffsets) {
			struct lp_build_context *uint_bld = &bld_base->uint_bld;
			struct lp_build_tgsi_soa_context *bld = lp_soa_context(bld_base);
			const struct tgsi_texture_offset * off = inst->TexOffsets;

			assert(inst->Texture.NumOffsets == 1);

			switch (target) {
			case TGSI_TEXTURE_3D:
				address[2] = lp_build_add(uint_bld, address[2],
						bld->immediates[off->Index][off->SwizzleZ]);
				/* fall through */
			case TGSI_TEXTURE_2D:
			case TGSI_TEXTURE_SHADOW2D:
			case TGSI_TEXTURE_RECT:
			case TGSI_TEXTURE_SHADOWRECT:
			case TGSI_TEXTURE_2D_ARRAY:
			case TGSI_TEXTURE_SHADOW2D_ARRAY:
				address[1] =
					lp_build_add(uint_bld, address[1],
						bld->immediates[off->Index][off->SwizzleY]);
				/* fall through */
			case TGSI_TEXTURE_1D:
			case TGSI_TEXTURE_SHADOW1D:
			case TGSI_TEXTURE_1D_ARRAY:
			case TGSI_TEXTURE_SHADOW1D_ARRAY:
				address[0] =
					lp_build_add(uint_bld, address[0],
						bld->immediates[off->Index][off->SwizzleX]);
				break;
				/* texture offsets do not apply to other texture targets */
			}
		}

		emit_data->dst_type = LLVMVectorType(
			LLVMInt32TypeInContext(bld_base->base.gallivm->context),
			4);

		emit_data->arg_count = 3;
	} else {
		/* Sampler */
		emit_data->args[2] = si_shader_ctx->samplers[sampler_index];

		emit_data->dst_type = LLVMVectorType(
			LLVMFloatTypeInContext(bld_base->base.gallivm->context),
			4);

		emit_data->arg_count = 4;
	}

	/* Dimensions */
	emit_data->args[emit_data->arg_count - 1] =
		lp_build_const_int32(bld_base->base.gallivm, target);

	/* Pad to power of two vector */
	while (count < util_next_power_of_two(count))
		address[count++] = LLVMGetUndef(LLVMInt32TypeInContext(gallivm->context));

	emit_data->args[0] = lp_build_gather_values(gallivm, address, count);
}

static void build_tex_intrinsic(const struct lp_build_tgsi_action * action,
				struct lp_build_tgsi_context * bld_base,
				struct lp_build_emit_data * emit_data)
{
	struct lp_build_context * base = &bld_base->base;
	char intr_name[127];

	if (emit_data->inst->Texture.Texture == TGSI_TEXTURE_BUFFER) {
		emit_data->output[emit_data->chan] = build_intrinsic(
			base->gallivm->builder,
			"llvm.SI.vs.load.input", emit_data->dst_type,
			emit_data->args, emit_data->arg_count,
			LLVMReadNoneAttribute | LLVMNoUnwindAttribute);
		return;
	}

	sprintf(intr_name, "%sv%ui32", action->intr_name,
		LLVMGetVectorSize(LLVMTypeOf(emit_data->args[0])));

	emit_data->output[emit_data->chan] = build_intrinsic(
		base->gallivm->builder, intr_name, emit_data->dst_type,
		emit_data->args, emit_data->arg_count,
		LLVMReadNoneAttribute | LLVMNoUnwindAttribute);
}

static void txq_fetch_args(
	struct lp_build_tgsi_context * bld_base,
	struct lp_build_emit_data * emit_data)
{
	struct si_shader_context *si_shader_ctx = si_shader_context(bld_base);
	const struct tgsi_full_instruction *inst = emit_data->inst;
	struct gallivm_state *gallivm = bld_base->base.gallivm;

	if (inst->Texture.Texture == TGSI_TEXTURE_BUFFER) {
		LLVMTypeRef i32 = LLVMInt32TypeInContext(gallivm->context);
		LLVMTypeRef v8i32 = LLVMVectorType(i32, 8);

		/* Read the size from the buffer descriptor directly. */
		LLVMValueRef size = si_shader_ctx->resources[inst->Src[1].Register.Index];
		size = LLVMBuildBitCast(gallivm->builder, size, v8i32, "");
		size = LLVMBuildExtractElement(gallivm->builder, size,
					      lp_build_const_int32(gallivm, 2), "");
		emit_data->args[0] = size;
		return;
	}

	/* Mip level */
	emit_data->args[0] = lp_build_emit_fetch(bld_base, inst, 0, TGSI_CHAN_X);

	/* Resource */
	emit_data->args[1] = si_shader_ctx->resources[inst->Src[1].Register.Index];

	/* Dimensions */
	emit_data->args[2] = lp_build_const_int32(bld_base->base.gallivm,
						  inst->Texture.Texture);

	emit_data->arg_count = 3;

	emit_data->dst_type = LLVMVectorType(
		LLVMInt32TypeInContext(bld_base->base.gallivm->context),
		4);
}

static void build_txq_intrinsic(const struct lp_build_tgsi_action * action,
				struct lp_build_tgsi_context * bld_base,
				struct lp_build_emit_data * emit_data)
{
	if (emit_data->inst->Texture.Texture == TGSI_TEXTURE_BUFFER) {
		/* Just return the buffer size. */
		emit_data->output[emit_data->chan] = emit_data->args[0];
		return;
	}

	build_tgsi_intrinsic_nomem(action, bld_base, emit_data);
}

#if HAVE_LLVM >= 0x0304

static void si_llvm_emit_ddxy(
	const struct lp_build_tgsi_action * action,
	struct lp_build_tgsi_context * bld_base,
	struct lp_build_emit_data * emit_data)
{
	struct si_shader_context *si_shader_ctx = si_shader_context(bld_base);
	struct gallivm_state *gallivm = bld_base->base.gallivm;
	struct lp_build_context * base = &bld_base->base;
	const struct tgsi_full_instruction *inst = emit_data->inst;
	unsigned opcode = inst->Instruction.Opcode;
	LLVMValueRef indices[2];
	LLVMValueRef store_ptr, load_ptr0, load_ptr1;
	LLVMValueRef tl, trbl, result[4];
	LLVMTypeRef i32;
	unsigned swizzle[4];
	unsigned c;

	i32 = LLVMInt32TypeInContext(gallivm->context);

	indices[0] = bld_base->uint_bld.zero;
	indices[1] = build_intrinsic(gallivm->builder, "llvm.SI.tid", i32,
				     NULL, 0, LLVMReadNoneAttribute);
	store_ptr = LLVMBuildGEP(gallivm->builder, si_shader_ctx->ddxy_lds,
				 indices, 2, "");

	indices[1] = LLVMBuildAnd(gallivm->builder, indices[1],
				  lp_build_const_int32(gallivm, 0xfffffffc), "");
	load_ptr0 = LLVMBuildGEP(gallivm->builder, si_shader_ctx->ddxy_lds,
				 indices, 2, "");

	indices[1] = LLVMBuildAdd(gallivm->builder, indices[1],
				  lp_build_const_int32(gallivm,
						       opcode == TGSI_OPCODE_DDX ? 1 : 2),
				  "");
	load_ptr1 = LLVMBuildGEP(gallivm->builder, si_shader_ctx->ddxy_lds,
				 indices, 2, "");

	for (c = 0; c < 4; ++c) {
		unsigned i;

		swizzle[c] = tgsi_util_get_full_src_register_swizzle(&inst->Src[0], c);
		for (i = 0; i < c; ++i) {
			if (swizzle[i] == swizzle[c]) {
				result[c] = result[i];
				break;
			}
		}
		if (i != c)
			continue;

		LLVMBuildStore(gallivm->builder,
			       LLVMBuildBitCast(gallivm->builder,
						lp_build_emit_fetch(bld_base, inst, 0, c),
						i32, ""),
			       store_ptr);

		tl = LLVMBuildLoad(gallivm->builder, load_ptr0, "");
		tl = LLVMBuildBitCast(gallivm->builder, tl, base->elem_type, "");

		trbl = LLVMBuildLoad(gallivm->builder, load_ptr1, "");
		trbl = LLVMBuildBitCast(gallivm->builder, trbl,	base->elem_type, "");

		result[c] = LLVMBuildFSub(gallivm->builder, trbl, tl, "");
	}

	emit_data->output[0] = lp_build_gather_values(gallivm, result, 4);
}

#endif /* HAVE_LLVM >= 0x0304 */

static const struct lp_build_tgsi_action tex_action = {
	.fetch_args = tex_fetch_args,
	.emit = build_tex_intrinsic,
	.intr_name = "llvm.SI.sample."
};

static const struct lp_build_tgsi_action txb_action = {
	.fetch_args = tex_fetch_args,
	.emit = build_tex_intrinsic,
	.intr_name = "llvm.SI.sampleb."
};

#if HAVE_LLVM >= 0x0304
static const struct lp_build_tgsi_action txd_action = {
	.fetch_args = tex_fetch_args,
	.emit = build_tex_intrinsic,
	.intr_name = "llvm.SI.sampled."
};
#endif

static const struct lp_build_tgsi_action txf_action = {
	.fetch_args = tex_fetch_args,
	.emit = build_tex_intrinsic,
	.intr_name = "llvm.SI.imageload."
};

static const struct lp_build_tgsi_action txl_action = {
	.fetch_args = tex_fetch_args,
	.emit = build_tex_intrinsic,
	.intr_name = "llvm.SI.samplel."
};

static const struct lp_build_tgsi_action txq_action = {
	.fetch_args = txq_fetch_args,
	.emit = build_txq_intrinsic,
	.intr_name = "llvm.SI.resinfo"
};

static void create_meta_data(struct si_shader_context *si_shader_ctx)
{
	struct gallivm_state *gallivm = si_shader_ctx->radeon_bld.soa.bld_base.base.gallivm;
	LLVMValueRef args[3];

	args[0] = LLVMMDStringInContext(gallivm->context, "const", 5);
	args[1] = 0;
	args[2] = lp_build_const_int32(gallivm, 1);

	si_shader_ctx->const_md = LLVMMDNodeInContext(gallivm->context, args, 3);
}

static void create_function(struct si_shader_context *si_shader_ctx)
{
	struct lp_build_tgsi_context *bld_base = &si_shader_ctx->radeon_bld.soa.bld_base;
	struct gallivm_state *gallivm = bld_base->base.gallivm;
	LLVMTypeRef params[21], f32, i8, i32, v2i32, v3i32;
	unsigned i, last_sgpr, num_params;

	i8 = LLVMInt8TypeInContext(gallivm->context);
	i32 = LLVMInt32TypeInContext(gallivm->context);
	f32 = LLVMFloatTypeInContext(gallivm->context);
	v2i32 = LLVMVectorType(i32, 2);
	v3i32 = LLVMVectorType(i32, 3);

	params[SI_PARAM_CONST] = LLVMPointerType(
		LLVMArrayType(LLVMVectorType(i8, 16), NUM_CONST_BUFFERS), CONST_ADDR_SPACE);
	/* We assume at most 16 textures per program at the moment.
	 * This need probably need to be changed to support bindless textures */
	params[SI_PARAM_SAMPLER] = LLVMPointerType(
		LLVMArrayType(LLVMVectorType(i8, 16), NUM_SAMPLER_VIEWS), CONST_ADDR_SPACE);
	params[SI_PARAM_RESOURCE] = LLVMPointerType(
		LLVMArrayType(LLVMVectorType(i8, 32), NUM_SAMPLER_STATES), CONST_ADDR_SPACE);

	switch (si_shader_ctx->type) {
	case TGSI_PROCESSOR_VERTEX:
		params[SI_PARAM_VERTEX_BUFFER] = params[SI_PARAM_CONST];
		params[SI_PARAM_SO_BUFFER] = params[SI_PARAM_CONST];
		params[SI_PARAM_START_INSTANCE] = i32;
		num_params = SI_PARAM_START_INSTANCE+1;

		/* The locations of the other parameters are assigned dynamically. */

		/* Streamout SGPRs. */
		if (si_shader_ctx->shader->selector->so.num_outputs) {
			params[si_shader_ctx->param_streamout_config = num_params++] = i32;
			params[si_shader_ctx->param_streamout_write_index = num_params++] = i32;
		}
		/* A streamout buffer offset is loaded if the stride is non-zero. */
		for (i = 0; i < 4; i++) {
			if (!si_shader_ctx->shader->selector->so.stride[i])
				continue;

			params[si_shader_ctx->param_streamout_offset[i] = num_params++] = i32;
		}

		last_sgpr = num_params-1;

		/* VGPRs */
		params[si_shader_ctx->param_vertex_id = num_params++] = i32;
		params[num_params++] = i32; /* unused*/
		params[num_params++] = i32; /* unused */
		params[si_shader_ctx->param_instance_id = num_params++] = i32;
		break;

	case TGSI_PROCESSOR_FRAGMENT:
		params[SI_PARAM_ALPHA_REF] = f32;
		params[SI_PARAM_PRIM_MASK] = i32;
		last_sgpr = SI_PARAM_PRIM_MASK;
		params[SI_PARAM_PERSP_SAMPLE] = v2i32;
		params[SI_PARAM_PERSP_CENTER] = v2i32;
		params[SI_PARAM_PERSP_CENTROID] = v2i32;
		params[SI_PARAM_PERSP_PULL_MODEL] = v3i32;
		params[SI_PARAM_LINEAR_SAMPLE] = v2i32;
		params[SI_PARAM_LINEAR_CENTER] = v2i32;
		params[SI_PARAM_LINEAR_CENTROID] = v2i32;
		params[SI_PARAM_LINE_STIPPLE_TEX] = f32;
		params[SI_PARAM_POS_X_FLOAT] = f32;
		params[SI_PARAM_POS_Y_FLOAT] = f32;
		params[SI_PARAM_POS_Z_FLOAT] = f32;
		params[SI_PARAM_POS_W_FLOAT] = f32;
		params[SI_PARAM_FRONT_FACE] = f32;
		params[SI_PARAM_ANCILLARY] = f32;
		params[SI_PARAM_SAMPLE_COVERAGE] = f32;
		params[SI_PARAM_POS_FIXED_PT] = f32;
		num_params = SI_PARAM_POS_FIXED_PT+1;
		break;

	default:
		assert(0 && "unimplemented shader");
		return;
	}

	assert(num_params <= Elements(params));
	radeon_llvm_create_func(&si_shader_ctx->radeon_bld, params, num_params);
	radeon_llvm_shader_type(si_shader_ctx->radeon_bld.main_fn, si_shader_ctx->type);

	for (i = 0; i <= last_sgpr; ++i) {
		LLVMValueRef P = LLVMGetParam(si_shader_ctx->radeon_bld.main_fn, i);
		switch (i) {
		default:
			LLVMAddAttribute(P, LLVMInRegAttribute);
			break;
#if HAVE_LLVM >= 0x0304
		/* We tell llvm that array inputs are passed by value to allow Sinking pass
		 * to move load. Inputs are constant so this is fine. */
		case SI_PARAM_CONST:
		case SI_PARAM_SAMPLER:
		case SI_PARAM_RESOURCE:
			LLVMAddAttribute(P, LLVMByValAttribute);
			break;
#endif
		}
	}

#if HAVE_LLVM >= 0x0304
	if (bld_base->info->opcode_count[TGSI_OPCODE_DDX] > 0 ||
	    bld_base->info->opcode_count[TGSI_OPCODE_DDY] > 0)
		si_shader_ctx->ddxy_lds =
			LLVMAddGlobalInAddressSpace(gallivm->module,
						    LLVMArrayType(i32, 64),
						    "ddxy_lds",
						    LOCAL_ADDR_SPACE);
#endif
}

static void preload_constants(struct si_shader_context *si_shader_ctx)
{
	struct lp_build_tgsi_context * bld_base = &si_shader_ctx->radeon_bld.soa.bld_base;
	struct gallivm_state * gallivm = bld_base->base.gallivm;
	const struct tgsi_shader_info * info = bld_base->info;
	unsigned buf;
	LLVMValueRef ptr = LLVMGetParam(si_shader_ctx->radeon_bld.main_fn, SI_PARAM_CONST);

	for (buf = 0; buf < NUM_CONST_BUFFERS; buf++) {
		unsigned i, num_const = info->const_file_max[buf] + 1;

		if (num_const == 0)
			continue;

		/* Allocate space for the constant values */
		si_shader_ctx->constants[buf] = CALLOC(num_const * 4, sizeof(LLVMValueRef));

		/* Load the resource descriptor */
		si_shader_ctx->const_resource[buf] =
			build_indexed_load(si_shader_ctx, ptr, lp_build_const_int32(gallivm, buf));

		/* Load the constants, we rely on the code sinking to do the rest */
		for (i = 0; i < num_const * 4; ++i) {
			LLVMValueRef args[2] = {
				si_shader_ctx->const_resource[buf],
				lp_build_const_int32(gallivm, i * 4)
			};
			si_shader_ctx->constants[buf][i] =
					build_intrinsic(gallivm->builder, "llvm.SI.load.const",
							bld_base->base.elem_type, args, 2,
							LLVMReadNoneAttribute | LLVMNoUnwindAttribute);
		}
	}
}

static void preload_samplers(struct si_shader_context *si_shader_ctx)
{
	struct lp_build_tgsi_context * bld_base = &si_shader_ctx->radeon_bld.soa.bld_base;
	struct gallivm_state * gallivm = bld_base->base.gallivm;
	const struct tgsi_shader_info * info = bld_base->info;

	unsigned i, num_samplers = info->file_max[TGSI_FILE_SAMPLER] + 1;

	LLVMValueRef res_ptr, samp_ptr;
	LLVMValueRef offset;

	if (num_samplers == 0)
		return;

	/* Allocate space for the values */
	si_shader_ctx->resources = CALLOC(NUM_SAMPLER_VIEWS, sizeof(LLVMValueRef));
	si_shader_ctx->samplers = CALLOC(num_samplers, sizeof(LLVMValueRef));

	res_ptr = LLVMGetParam(si_shader_ctx->radeon_bld.main_fn, SI_PARAM_RESOURCE);
	samp_ptr = LLVMGetParam(si_shader_ctx->radeon_bld.main_fn, SI_PARAM_SAMPLER);

	/* Load the resources and samplers, we rely on the code sinking to do the rest */
	for (i = 0; i < num_samplers; ++i) {
		/* Resource */
		offset = lp_build_const_int32(gallivm, i);
		si_shader_ctx->resources[i] = build_indexed_load(si_shader_ctx, res_ptr, offset);

		/* Sampler */
		offset = lp_build_const_int32(gallivm, i);
		si_shader_ctx->samplers[i] = build_indexed_load(si_shader_ctx, samp_ptr, offset);

		/* FMASK resource */
		if (info->is_msaa_sampler[i]) {
			offset = lp_build_const_int32(gallivm, FMASK_TEX_OFFSET + i);
			si_shader_ctx->resources[FMASK_TEX_OFFSET + i] =
				build_indexed_load(si_shader_ctx, res_ptr, offset);
		}
	}
}

static void preload_streamout_buffers(struct si_shader_context *si_shader_ctx)
{
	struct lp_build_tgsi_context * bld_base = &si_shader_ctx->radeon_bld.soa.bld_base;
	struct gallivm_state * gallivm = bld_base->base.gallivm;
	unsigned i;

	if (!si_shader_ctx->shader->selector->so.num_outputs)
		return;

	LLVMValueRef buf_ptr = LLVMGetParam(si_shader_ctx->radeon_bld.main_fn,
					    SI_PARAM_SO_BUFFER);

	/* Load the resources, we rely on the code sinking to do the rest */
	for (i = 0; i < 4; ++i) {
		if (si_shader_ctx->shader->selector->so.stride[i]) {
			LLVMValueRef offset = lp_build_const_int32(gallivm, i);

			si_shader_ctx->so_buffers[i] = build_indexed_load(si_shader_ctx, buf_ptr, offset);
		}
	}
}

int si_compile_llvm(struct r600_context *rctx, struct si_pipe_shader *shader,
							LLVMModuleRef mod)
{
	unsigned i;
	uint32_t *ptr;
	struct radeon_llvm_binary binary;
	bool dump = r600_can_dump_shader(&rctx->screen->b,
			shader->selector ? shader->selector->tokens : NULL);
	memset(&binary, 0, sizeof(binary));
	radeon_llvm_compile(mod, &binary,
		r600_get_llvm_processor_name(rctx->screen->b.family), dump);
	if (dump && ! binary.disassembled) {
		fprintf(stderr, "SI CODE:\n");
		for (i = 0; i < binary.code_size; i+=4 ) {
			fprintf(stderr, "%02x%02x%02x%02x\n", binary.code[i + 3],
				binary.code[i + 2], binary.code[i + 1],
				binary.code[i]);
		}
	}

	/* XXX: We may be able to emit some of these values directly rather than
	 * extracting fields to be emitted later.
	 */
	for (i = 0; i < binary.config_size; i+= 8) {
		unsigned reg = util_le32_to_cpu(*(uint32_t*)(binary.config + i));
		unsigned value = util_le32_to_cpu(*(uint32_t*)(binary.config + i + 4));
		switch (reg) {
		case R_00B028_SPI_SHADER_PGM_RSRC1_PS:
		case R_00B128_SPI_SHADER_PGM_RSRC1_VS:
		case R_00B228_SPI_SHADER_PGM_RSRC1_GS:
		case R_00B848_COMPUTE_PGM_RSRC1:
			shader->num_sgprs = (G_00B028_SGPRS(value) + 1) * 8;
			shader->num_vgprs = (G_00B028_VGPRS(value) + 1) * 4;
			break;
		case R_00B02C_SPI_SHADER_PGM_RSRC2_PS:
			shader->lds_size = G_00B02C_EXTRA_LDS_SIZE(value);
			break;
		case R_00B84C_COMPUTE_PGM_RSRC2:
			shader->lds_size = G_00B84C_LDS_SIZE(value);
			break;
		case R_0286CC_SPI_PS_INPUT_ENA:
			shader->spi_ps_input_ena = value;
			break;
		default:
			fprintf(stderr, "Warning: Compiler emitted unknown "
				"config register: 0x%x\n", reg);
			break;
		}
	}

	/* copy new shader */
	r600_resource_reference(&shader->bo, NULL);
	shader->bo = r600_resource_create_custom(rctx->b.b.screen, PIPE_USAGE_IMMUTABLE,
					       binary.code_size);
	if (shader->bo == NULL) {
		return -ENOMEM;
	}

	ptr = (uint32_t*)rctx->b.ws->buffer_map(shader->bo->cs_buf, rctx->b.rings.gfx.cs, PIPE_TRANSFER_WRITE);
	if (0 /*R600_BIG_ENDIAN*/) {
		for (i = 0; i < binary.code_size / 4; ++i) {
			ptr[i] = util_bswap32(*(uint32_t*)(binary.code + i*4));
		}
	} else {
		memcpy(ptr, binary.code, binary.code_size);
	}
	rctx->b.ws->buffer_unmap(shader->bo->cs_buf);

	free(binary.code);
	free(binary.config);

	return 0;
}

int si_pipe_shader_create(
	struct pipe_context *ctx,
	struct si_pipe_shader *shader)
{
	struct r600_context *rctx = (struct r600_context*)ctx;
	struct si_pipe_shader_selector *sel = shader->selector;
	struct si_shader_context si_shader_ctx;
	struct tgsi_shader_info shader_info;
	struct lp_build_tgsi_context * bld_base;
	LLVMModuleRef mod;
	int r = 0;
	bool dump = r600_can_dump_shader(&rctx->screen->b, shader->selector->tokens);

	assert(shader->shader.noutput == 0);
	assert(shader->shader.ninterp == 0);
	assert(shader->shader.ninput == 0);

	memset(&si_shader_ctx, 0, sizeof(si_shader_ctx));
	radeon_llvm_context_init(&si_shader_ctx.radeon_bld);
	bld_base = &si_shader_ctx.radeon_bld.soa.bld_base;

	tgsi_scan_shader(sel->tokens, &shader_info);

	shader->shader.uses_kill = shader_info.uses_kill;
	shader->shader.uses_instanceid = shader_info.uses_instanceid;
	bld_base->info = &shader_info;
	bld_base->emit_fetch_funcs[TGSI_FILE_CONSTANT] = fetch_constant;
	bld_base->emit_epilogue = si_llvm_emit_epilogue;

	bld_base->op_actions[TGSI_OPCODE_TEX] = tex_action;
	bld_base->op_actions[TGSI_OPCODE_TXB] = txb_action;
#if HAVE_LLVM >= 0x0304
	bld_base->op_actions[TGSI_OPCODE_TXD] = txd_action;
#endif
	bld_base->op_actions[TGSI_OPCODE_TXF] = txf_action;
	bld_base->op_actions[TGSI_OPCODE_TXL] = txl_action;
	bld_base->op_actions[TGSI_OPCODE_TXP] = tex_action;
	bld_base->op_actions[TGSI_OPCODE_TXQ] = txq_action;

#if HAVE_LLVM >= 0x0304
	bld_base->op_actions[TGSI_OPCODE_DDX].emit = si_llvm_emit_ddxy;
	bld_base->op_actions[TGSI_OPCODE_DDY].emit = si_llvm_emit_ddxy;
#endif

	si_shader_ctx.radeon_bld.load_input = declare_input;
	si_shader_ctx.radeon_bld.load_system_value = declare_system_value;
	si_shader_ctx.tokens = sel->tokens;
	tgsi_parse_init(&si_shader_ctx.parse, si_shader_ctx.tokens);
	si_shader_ctx.shader = shader;
	si_shader_ctx.type = si_shader_ctx.parse.FullHeader.Processor.Processor;

	create_meta_data(&si_shader_ctx);
	create_function(&si_shader_ctx);
	preload_constants(&si_shader_ctx);
	preload_samplers(&si_shader_ctx);
	preload_streamout_buffers(&si_shader_ctx);

	shader->shader.nr_cbufs = rctx->framebuffer.nr_cbufs;

	/* Dump TGSI code before doing TGSI->LLVM conversion in case the
	 * conversion fails. */
	if (dump) {
		tgsi_dump(sel->tokens, 0);
		si_dump_streamout(&sel->so);
	}

	if (!lp_build_tgsi_llvm(bld_base, sel->tokens)) {
		fprintf(stderr, "Failed to translate shader from TGSI to LLVM\n");
		for (int i = 0; i < NUM_CONST_BUFFERS; i++)
			FREE(si_shader_ctx.constants[i]);
		FREE(si_shader_ctx.resources);
		FREE(si_shader_ctx.samplers);
		return -EINVAL;
	}

	radeon_llvm_finalize_module(&si_shader_ctx.radeon_bld);

	mod = bld_base->base.gallivm->module;
	r = si_compile_llvm(rctx, shader, mod);

	radeon_llvm_dispose(&si_shader_ctx.radeon_bld);
	tgsi_parse_free(&si_shader_ctx.parse);

	for (int i = 0; i < NUM_CONST_BUFFERS; i++)
		FREE(si_shader_ctx.constants[i]);
	FREE(si_shader_ctx.resources);
	FREE(si_shader_ctx.samplers);

	return r;
}

void si_pipe_shader_destroy(struct pipe_context *ctx, struct si_pipe_shader *shader)
{
	r600_resource_reference(&shader->bo, NULL);
}
