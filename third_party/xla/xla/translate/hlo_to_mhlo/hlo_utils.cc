/* Copyright 2019 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// This file defines helpers useful when creating or manipulating lhlo/hlo.

#include "xla/translate/hlo_to_mhlo/hlo_utils.h"

#include <cstddef>
#include <type_traits>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "mlir/IR/AffineMap.h"  // from @llvm-project
#include "mlir/IR/Attributes.h"  // from @llvm-project
#include "mlir/IR/BuiltinTypes.h"  // from @llvm-project
#include "mlir/IR/TypeUtilities.h"  // from @llvm-project
#include "xla/literal.h"
#include "xla/mlir/utils/type_util.h"
#include "xla/mlir_hlo/lhlo/IR/lhlo_ops.h"
#include "xla/primitive_util.h"
#include "xla/service/llvm_ir/llvm_util.h"
#include "xla/types.h"
#include "xla/util.h"
#include "xla/xla_data.pb.h"

namespace xla {
namespace {

using mlir::AffineMap;
using mlir::Builder;
using mlir::DenseElementsAttr;
using mlir::ShapedType;
using xla::LiteralBase;
using xla::StatusOr;

template <typename CppType>
::mlir::DenseElementsAttr CreateDenseAttrFromLiteral(
    const ShapedType& type, const LiteralBase& literal) {
  if constexpr (std::is_same_v<CppType, u4> || std::is_same_v<CppType, s4>) {
    // DenseElementsAttr::get() does not support being passed an i4 array.
    // Instead, create buffer of padded i4 values and call
    // DenseElementsAttr::getFromRawBuffer()
    auto data_span = literal.data<CppType>();
    std::vector<char> int4_padded_data;
    int4_padded_data.reserve(literal.element_count());
    for (size_t i = 0; i < literal.element_count(); i++) {
      int4_padded_data.push_back(static_cast<char>(data_span[i]));
    }
    return ::mlir::DenseElementsAttr::getFromRawBuffer(type, int4_padded_data);
  } else {
    auto data_span = literal.data<CppType>();
    return ::mlir::DenseElementsAttr::get(
        type, llvm::ArrayRef(data_span.data(), data_span.size()));
  }
}

absl::StatusOr<AffineMap> GetPermutationIfAvailable(const Shape& shape,
                                                    mlir::Builder builder) {
  // N.B. IsMonotonicWithDim0Major ignores tiling, and I can't change it because
  // some XLA code relies on it treating tiled layouts as equivalent to untiled
  // layouts, so the check to rule out tiling has to come /before/ the
  // early-return branch, or we'd miss tiled monotonic layouts.
  if (!shape.layout().tiles().empty()) {
    return Internal("Tiled layouts are not yet supported");
  }
  if (!shape.has_layout() ||
      LayoutUtil::IsMonotonicWithDim0Major(shape.layout())) {
    return AffineMap();
  }
  if (!shape.is_static()) {
    return Internal("Permutations for dynamic shapes are not yet supported");
  }
  int64_t accumulated_stride = 1;
  llvm::SmallVector<int64_t, 4> strides(shape.rank(), 1);
  for (int64_t dim : LayoutUtil::MinorToMajor(shape)) {
    strides[dim] = accumulated_stride;
    accumulated_stride *= shape.dimensions(dim);
  }
  if (accumulated_stride == 0) {
    return AffineMap();
  }
  return makeStridedLinearLayoutMap(strides, /*offset=*/0,
                                    builder.getContext());
}
}  // namespace

absl::StatusOr<mlir::MemRefType> ConvertTensorShapeToMemRefType(
    const Shape& shape, mlir::Builder builder) {
  auto element_type_or =
      ConvertPrimitiveTypeToMlirType(shape.element_type(), builder);
  if (!element_type_or.ok()) return element_type_or.status();

  using mlir::MemRefType;
  auto dimensions = shape.dimensions();
  llvm::SmallVector<int64_t, 4> array(dimensions.begin(), dimensions.end());
  auto permutation_or = GetPermutationIfAvailable(shape, builder);
  if (!permutation_or.ok()) return permutation_or.status();
  return MemRefType::get(array, element_type_or.value(),
                         permutation_or.value());
}

absl::StatusOr<mlir::DenseElementsAttr> CreateDenseElementsAttrFromLiteral(
    const LiteralBase& literal, Builder builder) {
  TF_ASSIGN_OR_RETURN(auto type,
                      ConvertTensorShapeToType<mlir::RankedTensorType>(
                          literal.shape(), builder));

  // TODO(hinsu): Support remaining XLA primitive types.
  auto element_type = literal.shape().element_type();
  return primitive_util::PrimitiveTypeSwitch<StatusOr<mlir::DenseElementsAttr>>(
      [&](auto primitive_type_constant)
          -> absl::StatusOr<mlir::DenseElementsAttr> {
        if constexpr (primitive_util::IsArrayType(primitive_type_constant)) {
          return CreateDenseAttrFromLiteral<
              primitive_util::NativeTypeOf<primitive_type_constant>>(type,
                                                                     literal);
        }
        return Internal("Unsupported type: %s",
                        PrimitiveType_Name(element_type));
      },
      element_type);
}

absl::StatusOr<int> GetElementTypeBytes(mlir::Type type) {
  if (type.isInteger(1)) {
    return 1;
  }
  if (auto complex_type = type.dyn_cast<mlir::ComplexType>()) {
    TF_ASSIGN_OR_RETURN(int bytes,
                        GetElementTypeBytes(complex_type.getElementType()));
    return bytes * 2;
  }
  int width = type.getIntOrFloatBitWidth();
  TF_RET_CHECK(width % 8 == 0);
  return width / 8;
}

mlir::DenseIntElementsAttr CreateDenseIntElementsAttrFromVector(
    const llvm::ArrayRef<int64_t> vector, mlir::Builder builder,
    llvm::ArrayRef<int64_t> shape) {
  return mlir::DenseIntElementsAttr::get(
      mlir::RankedTensorType::get(shape.empty() ? vector.size() : shape,
                                  builder.getIntegerType(64)),
      vector);
}

mlir::mhlo::GatherDimensionNumbersAttr CreateGatherDimensionNumbers(
    const GatherDimensionNumbers& input, mlir::Builder builder) {
  auto get_i64_array = [](absl::Span<const int64_t> container) {
    return llvm::ArrayRef<int64_t>{container.data(), container.size()};
  };
  return mlir::mhlo::GatherDimensionNumbersAttr::get(
      builder.getContext(), get_i64_array(input.offset_dims()),
      get_i64_array(input.collapsed_slice_dims()),
      get_i64_array(input.start_index_map()), input.index_vector_dim());
}

absl::StatusOr<::xla::HloOpcode> MhloToHloOpcode(mlir::Operation* op) {
  using mlir::isa;

  if (isa<mlir::mhlo::ConstantOp, mlir::lmhlo::ConstantOp>(op)) {
    return xla::HloOpcode::kConstant;
  } else if (isa<mlir::mhlo::IotaOp, mlir::lmhlo::IotaOp>(op)) {
    return xla::HloOpcode::kIota;
  } else if (isa<mlir::mhlo::ConvertOp, mlir::lmhlo::ConvertOp>(op)) {
    return xla::HloOpcode::kConvert;
  } else if (isa<mlir::mhlo::AddOp, mlir::lmhlo::AddOp>(op)) {
    return xla::HloOpcode::kAdd;
  } else if (isa<mlir::mhlo::Atan2Op, mlir::lmhlo::Atan2Op>(op)) {
    return xla::HloOpcode::kAtan2;
  } else if (isa<mlir::mhlo::DivOp, mlir::lmhlo::DivOp>(op)) {
    return xla::HloOpcode::kDivide;
  } else if (isa<mlir::mhlo::MaxOp, mlir::lmhlo::MaxOp>(op)) {
    return xla::HloOpcode::kMaximum;
  } else if (isa<mlir::mhlo::MinOp, mlir::lmhlo::MinOp>(op)) {
    return xla::HloOpcode::kMinimum;
  } else if (isa<mlir::mhlo::MulOp, mlir::lmhlo::MulOp>(op)) {
    return xla::HloOpcode::kMultiply;
  } else if (isa<mlir::mhlo::PowOp, mlir::lmhlo::PowOp>(op)) {
    return xla::HloOpcode::kPower;
  } else if (isa<mlir::mhlo::RemOp, mlir::lmhlo::RemOp>(op)) {
    return xla::HloOpcode::kRemainder;
  } else if (isa<mlir::mhlo::ShiftLeftOp, mlir::lmhlo::ShiftLeftOp>(op)) {
    return xla::HloOpcode::kShiftLeft;
  } else if (isa<mlir::mhlo::ShiftRightArithmeticOp,
                 mlir::lmhlo::ShiftRightArithmeticOp>(op)) {
    return xla::HloOpcode::kShiftRightArithmetic;
  } else if (isa<mlir::mhlo::ShiftRightLogicalOp,
                 mlir::lmhlo::ShiftRightLogicalOp>(op)) {
    return xla::HloOpcode::kShiftRightLogical;
  } else if (isa<mlir::mhlo::SubtractOp, mlir::lmhlo::SubtractOp>(op)) {
    return xla::HloOpcode::kSubtract;
  } else if (isa<mlir::mhlo::XorOp, mlir::lmhlo::XorOp>(op)) {
    return xla::HloOpcode::kXor;
  } else if (isa<mlir::mhlo::InfeedOp, mlir::lmhlo::InfeedOp>(op)) {
    return xla::HloOpcode::kInfeed;
  } else if (isa<mlir::mhlo::OutfeedOp, mlir::lmhlo::OutfeedOp>(op)) {
    return xla::HloOpcode::kOutfeed;
  } else if (isa<mlir::mhlo::SendOp>(op)) {
    return xla::HloOpcode::kSend;
  } else if (isa<mlir::mhlo::RecvOp>(op)) {
    return xla::HloOpcode::kRecv;
  } else if (isa<mlir::mhlo::ReplicaIdOp, mlir::lmhlo::ReplicaIdOp>(op)) {
    return xla::HloOpcode::kReplicaId;
  } else if (isa<mlir::mhlo::AfterAllOp>(op)) {
    return xla::HloOpcode::kAfterAll;
  } else if (isa<mlir::mhlo::AllReduceOp>(op)) {
    return xla::HloOpcode::kAllReduce;
  } else if (isa<mlir::mhlo::AllToAllOp>(op)) {
    return xla::HloOpcode::kAllToAll;
  } else if (isa<mlir::mhlo::TupleOp>(op)) {
    return xla::HloOpcode::kTuple;
  } else if (isa<mlir::mhlo::BatchNormGradOp, mlir::lmhlo::BatchNormGradOp>(
                 op)) {
    return xla::HloOpcode::kBatchNormGrad;
  } else if (isa<mlir::mhlo::BatchNormInferenceOp,
                 mlir::lmhlo::BatchNormInferenceOp>(op)) {
    return xla::HloOpcode::kBatchNormInference;
  } else if (isa<mlir::mhlo::BatchNormTrainingOp,
                 mlir::lmhlo::BatchNormTrainingOp>(op)) {
    return xla::HloOpcode::kBatchNormTraining;
  } else if (isa<mlir::mhlo::BitcastConvertOp, mlir::lmhlo::BitcastConvertOp>(
                 op)) {
    return xla::HloOpcode::kBitcastConvert;
  } else if (isa<mlir::mhlo::BroadcastOp, mlir::lmhlo::BroadcastOp>(op)) {
    return xla::HloOpcode::kBroadcast;
  } else if (isa<mlir::mhlo::CholeskyOp, mlir::lmhlo::CholeskyOp>(op)) {
    return xla::HloOpcode::kCholesky;
  } else if (isa<mlir::mhlo::ClampOp, mlir::lmhlo::ClampOp>(op)) {
    return xla::HloOpcode::kClamp;
  } else if (isa<mlir::mhlo::ConcatenateOp, mlir::lmhlo::ConcatenateOp>(op)) {
    return xla::HloOpcode::kConcatenate;
  } else if (isa<mlir::mhlo::ConvolutionOp, mlir::lmhlo::ConvolutionOp>(op)) {
    return xla::HloOpcode::kConvolution;
  } else if (isa<mlir::mhlo::SortOp, mlir::lmhlo::SortOp>(op)) {
    return xla::HloOpcode::kSort;
  } else if (isa<mlir::mhlo::TopKOp>(op)) {
    return xla::HloOpcode::kTopK;
  } else if (isa<mlir::mhlo::RngBitGeneratorOp>(op)) {
    return xla::HloOpcode::kRngBitGenerator;
  } else if (isa<mlir::mhlo::XlaRngGetAndUpdateStateOp>(op)) {
    return xla::HloOpcode::kRngGetAndUpdateState;
  } else if (isa<mlir::mhlo::FusionOp, mlir::lmhlo::FusionOp>(op)) {
    return xla::HloOpcode::kFusion;
  } else if (isa<mlir::mhlo::BitcastOp>(op)) {
    return xla::HloOpcode::kBitcast;
  } else if (isa<mlir::mhlo::AbsOp, mlir::lmhlo::AbsOp>(op)) {
    return xla::HloOpcode::kAbs;
  } else if (isa<mlir::mhlo::CbrtOp, mlir::lmhlo::CbrtOp>(op)) {
    return xla::HloOpcode::kCbrt;
  } else if (isa<mlir::mhlo::CeilOp, mlir::lmhlo::CeilOp>(op)) {
    return xla::HloOpcode::kCeil;
  } else if (isa<mlir::mhlo::ClzOp, mlir::lmhlo::ClzOp>(op)) {
    return xla::HloOpcode::kClz;
  } else if (isa<mlir::mhlo::CosineOp, mlir::lmhlo::CosineOp>(op)) {
    return xla::HloOpcode::kCos;
  } else if (isa<mlir::mhlo::ErfOp>(op)) {
    return xla::HloOpcode::kErf;
  } else if (isa<mlir::mhlo::ExpOp, mlir::lmhlo::ExpOp>(op)) {
    return xla::HloOpcode::kExp;
  } else if (isa<mlir::mhlo::Expm1Op, mlir::lmhlo::Expm1Op>(op)) {
    return xla::HloOpcode::kExpm1;
  } else if (isa<mlir::mhlo::FloorOp, mlir::lmhlo::FloorOp>(op)) {
    return xla::HloOpcode::kFloor;
  } else if (isa<mlir::mhlo::ImagOp, mlir::lmhlo::ImagOp>(op)) {
    return xla::HloOpcode::kImag;
  } else if (isa<mlir::mhlo::IsFiniteOp, mlir::lmhlo::IsFiniteOp>(op)) {
    return xla::HloOpcode::kIsFinite;
  } else if (isa<mlir::mhlo::LogOp, mlir::lmhlo::LogOp>(op)) {
    return xla::HloOpcode::kLog;
  } else if (isa<mlir::mhlo::Log1pOp, mlir::lmhlo::Log1pOp>(op)) {
    return xla::HloOpcode::kLog1p;
  } else if (isa<mlir::mhlo::LogisticOp>(op)) {
    return xla::HloOpcode::kLogistic;
  } else if (isa<mlir::mhlo::NotOp, mlir::lmhlo::NotOp>(op)) {
    return xla::HloOpcode::kNot;
  } else if (isa<mlir::mhlo::NegOp, mlir::lmhlo::NegOp>(op)) {
    return xla::HloOpcode::kNegate;
  } else if (isa<mlir::mhlo::PopulationCountOp, mlir::lmhlo::PopulationCountOp>(
                 op)) {
    return xla::HloOpcode::kPopulationCount;
  } else if (isa<mlir::mhlo::RealOp, mlir::lmhlo::RealOp>(op)) {
    return xla::HloOpcode::kReal;
  } else if (isa<mlir::mhlo::RoundOp, mlir::lmhlo::RoundOp>(op)) {
    return xla::HloOpcode::kRoundNearestAfz;
  } else if (isa<mlir::mhlo::RoundNearestEvenOp,
                 mlir::lmhlo::RoundNearestEvenOp>(op)) {
    return xla::HloOpcode::kRoundNearestEven;
  } else if (isa<mlir::mhlo::RsqrtOp, mlir::lmhlo::RsqrtOp>(op)) {
    return xla::HloOpcode::kRsqrt;
  } else if (isa<mlir::mhlo::SignOp, mlir::lmhlo::SignOp>(op)) {
    return xla::HloOpcode::kSign;
  } else if (isa<mlir::mhlo::SineOp, mlir::lmhlo::SineOp>(op)) {
    return xla::HloOpcode::kSin;
  } else if (isa<mlir::mhlo::SqrtOp, mlir::lmhlo::SqrtOp>(op)) {
    return xla::HloOpcode::kSqrt;
  } else if (isa<mlir::mhlo::TanOp, mlir::lmhlo::TanOp>(op)) {
    return xla::HloOpcode::kTan;
  } else if (isa<mlir::mhlo::TanhOp, mlir::lmhlo::TanhOp>(op)) {
    return xla::HloOpcode::kTanh;
  } else if (isa<mlir::mhlo::ComplexOp, mlir::lmhlo::ComplexOp>(op)) {
    return xla::HloOpcode::kComplex;
  } else if (isa<mlir::mhlo::AndOp, mlir::lmhlo::AndOp>(op)) {
    return xla::HloOpcode::kAnd;
  } else if (isa<mlir::mhlo::OrOp, mlir::lmhlo::OrOp>(op)) {
    return xla::HloOpcode::kOr;
  } else if (isa<mlir::mhlo::WhileOp, mlir::lmhlo::WhileOp>(op)) {
    return xla::HloOpcode::kWhile;
  } else if (isa<mlir::mhlo::ReduceOp, mlir::lmhlo::ReduceOp>(op)) {
    return xla::HloOpcode::kReduce;
  } else if (isa<mlir::mhlo::GetTupleElementOp>(op)) {
    return xla::HloOpcode::kGetTupleElement;
  } else if (isa<mlir::mhlo::CompareOp, mlir::lmhlo::CompareOp>(op)) {
    return xla::HloOpcode::kCompare;
  } else if (isa<mlir::mhlo::SliceOp, mlir::lmhlo::SliceOp>(op)) {
    return xla::HloOpcode::kSlice;
  } else if (isa<mlir::mhlo::DynamicSliceOp, mlir::lmhlo::DynamicSliceOp>(op)) {
    return xla::HloOpcode::kDynamicSlice;
  } else if (isa<mlir::mhlo::DynamicUpdateSliceOp,
                 mlir::lmhlo::DynamicUpdateSliceOp>(op)) {
    return xla::HloOpcode::kDynamicUpdateSlice;
  } else if (isa<mlir::mhlo::CollectivePermuteOp>(op)) {
    return xla::HloOpcode::kCollectivePermute;
  } else if (isa<mlir::mhlo::CopyOp, mlir::lmhlo::CopyOp>(op)) {
    return xla::HloOpcode::kCopy;
  } else if (isa<mlir::mhlo::CustomCallOp, mlir::lmhlo::CustomCallOp>(op)) {
    return xla::HloOpcode::kCustomCall;
  } else if (isa<mlir::mhlo::DotOp, mlir::lmhlo::DotOp>(op)) {
    return xla::HloOpcode::kDot;
  } else if (isa<mlir::mhlo::FftOp, mlir::lmhlo::FftOp>(op)) {
    return xla::HloOpcode::kFft;
  } else if (isa<mlir::mhlo::GatherOp, mlir::lmhlo::GatherOp>(op)) {
    return xla::HloOpcode::kGather;
  } else if (isa<mlir::mhlo::GetDimensionSizeOp>(op)) {
    return xla::HloOpcode::kGetDimensionSize;
  } else if (isa<mlir::mhlo::MapOp, mlir::lmhlo::MapOp>(op)) {
    return xla::HloOpcode::kMap;
  } else if (isa<mlir::mhlo::ReshapeOp, mlir::lmhlo::ReshapeOp>(op)) {
    return xla::HloOpcode::kReshape;
  } else if (isa<mlir::mhlo::DynamicReshapeOp>(op)) {
    return xla::HloOpcode::kDynamicReshape;
  } else if (isa<mlir::mhlo::ScatterOp, mlir::lmhlo::ScatterOp>(op)) {
    return xla::HloOpcode::kScatter;
  } else if (isa<mlir::mhlo::SelectOp, mlir::lmhlo::SelectOp>(op)) {
    return xla::HloOpcode::kSelect;
  } else if (isa<mlir::mhlo::SelectAndScatterOp,
                 mlir::lmhlo::SelectAndScatterOp>(op)) {
    return xla::HloOpcode::kSelectAndScatter;
  } else if (isa<mlir::mhlo::SetDimensionSizeOp>(op)) {
    return xla::HloOpcode::kSetDimensionSize;
  } else if (isa<mlir::mhlo::ReverseOp, mlir::lmhlo::ReverseOp>(op)) {
    return xla::HloOpcode::kReverse;
  } else if (isa<mlir::mhlo::PadOp, mlir::lmhlo::PadOp>(op)) {
    return xla::HloOpcode::kPad;
  } else if (isa<mlir::mhlo::TransposeOp, mlir::lmhlo::TransposeOp>(op)) {
    return xla::HloOpcode::kTranspose;
  } else if (isa<mlir::mhlo::TriangularSolveOp, mlir::lmhlo::TriangularSolveOp>(
                 op)) {
    return xla::HloOpcode::kTriangularSolve;
  } else if (isa<mlir::mhlo::ReduceWindowOp, mlir::lmhlo::ReduceWindowOp>(op)) {
    return xla::HloOpcode::kReduceWindow;
  } else if (isa<mlir::mhlo::ReducePrecisionOp, mlir::lmhlo::ReducePrecisionOp>(
                 op)) {
    return xla::HloOpcode::kReducePrecision;
  } else if (isa<mlir::mhlo::DotGeneralOp>(op)) {
    return xla::HloOpcode::kDot;
  } else if (isa<mlir::mhlo::BroadcastInDimOp, mlir::lmhlo::BroadcastInDimOp>(
                 op)) {
    return xla::HloOpcode::kBroadcast;
  } else {
    return Unimplemented("Unimplemented MHLO -> HloOpcode: %s",
                         llvm_ir::DumpToString(op));
  }
}
}  // namespace xla
