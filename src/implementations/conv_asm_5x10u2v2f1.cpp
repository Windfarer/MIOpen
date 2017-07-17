#include "miopen/algorithm_implementations.hpp"
#include "miopen/gcn_asm_utils.hpp"
#include "miopen/handle.hpp"

namespace miopen {
bool ConvAsm5x10u2v2f1::IsCorrect(const ImplementationSearchParameters& params) const
{
    if(!params.assembler_available)
    {
        return false;
    }

    const std::string name = params.GetStream().GetDeviceName();
    const bool device_is_gfx8_9_no_xnack =
        (name == "gfx800" || name == "gfx802" || name == "gfx803" || name == "gfx804" ||
         name == "gfx900");
    if(!device_is_gfx8_9_no_xnack)
    {
        return false;
    }
    if(!params.forward)
    {
        return false;
    }
    assert(params.weights_layout.length() == 0); // FIXME _weights_layout is not supported yet.

    // Min image + padding shall be not smaller than filter matrix.
    const int min_in_width  = params.kernel_size0 - params.pad0 * 2;
    const int min_in_height = params.kernel_size1 - params.pad1 * 2;
    // These two found experimentally.
    const int max_in_width  = 8192 - 1;
    const int max_in_height = 131077 - 1;

    return                            // Opt. Param   Restrictions in source
        params.pad0 >= 0              // -q   pad_w   // [0..5] for now FIXME
        && params.pad0 <= 5           //
        && params.pad1 >= 0           // -p   pad_h   // [0..5] for now FIXME
        && params.pad1 <= 5           //
        && params.kernel_stride0 == 2 // -u   inp_u   fixed
        && params.kernel_stride1 == 2 // -v   inp_v   fixed
        && params.kernel_size0 == 10  // -x   wei_w   fixed
        && params.kernel_size1 == 5   // -y   wei_h   fixed
        && params.n_inputs >= 1       // -c   wei_c   no upper limit
        && params.n_outputs % 16 == 0 // -k   wei_k   no upper limit
        && params.n_outputs >= 1 && params.in_width >= min_in_width             // -W   inp_w
        && params.in_width <= max_in_width && params.in_height >= min_in_height // -H   inp_h
        && params.in_height <= max_in_height &&
        params.in_layout == "NCHW"; //              hardcoded
    // && (params.forward ? params.weights_layout == "KCHW" : params.weights_layout == "CKHW" ) //
    // See fixme above.
}

static inline int AlignUp(int val, unsigned step)
{
    assert(step > 0);
    return ((val + step - 1) / step) * step;
}

ImplementationUsageDescription
ConvAsm5x10u2v2f1::PrepareForUsage(const ImplementationSearchParameters& params,
                                   const ExaustiveSearchResult& /*exaustive_search_result*/) const
{
    const int out_w =
        (params.in_width + params.pad0 * 2 + params.kernel_stride0 - params.kernel_size0) /
        params.kernel_stride0; // (inp_w + 2*pad_w + inp_u - wei_w) / inp_u
    const int out_h =
        (params.in_height + params.pad1 * 2 + params.kernel_stride1 - params.kernel_size1) /
        params.kernel_stride1; // (inp_h + 2*pad_h + inp_v - wei_h) / inp_v

    std::ostringstream options;
    GenerateClangDefsym(options, "inp_h", params.in_height);
    GenerateClangDefsym(options, "inp_w", params.in_width);
    GenerateClangDefsym(options, "wei_c", params.n_inputs);
    GenerateClangDefsym(options, "wei_k", params.n_outputs);
    GenerateClangDefsym(options, "wei_layout", 0); // 0: KCHW, 1: CKHW
    GenerateClangDefsym(options, "pad_w", params.pad0);
    GenerateClangDefsym(options, "pad_h", params.pad1);
    GenerateClangDefsym(
        options, "ROCM_METADATA_VERSION", (params.rmv == V1) ? 1 : ((params.rmv == V2) ? 2 : 3));

    KernelUsageDescription construction_params;
    construction_params.comp_options = options.str();

    construction_params.l_wk.push_back(64);
    construction_params.l_wk.push_back(8);
    construction_params.l_wk.push_back(1);

    // global-work = [align(out_w,64), (align(out_h,4)/4)*align(wei_k/2,8), batch_n]
    construction_params.g_wk.push_back(AlignUp(out_w, 64));
    construction_params.g_wk.push_back(AlignUp(out_h, 4) / 4 * AlignUp(params.n_outputs / 2, 8));
    construction_params.g_wk.push_back(params.batch_sz);

    construction_params.kernel_file = "conv5x10u2v2f1.s";
    construction_params.kernel_name = "conv5x10u2v2f1";

    ImplementationUsageDescription result;
    result.construction_params.push_back(construction_params);
    return result;
}
} // namespace miopen
