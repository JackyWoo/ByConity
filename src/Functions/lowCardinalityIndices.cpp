#include <Functions/IFunction.h>
#include <Functions/FunctionFactory.h>
#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/DataTypeLowCardinality.h>
#include <Columns/ColumnsNumber.h>
#include <Columns/ColumnLowCardinality.h>
#include <Common/typeid_cast.h>


namespace DB
{
namespace ErrorCodes
{
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
}

namespace
{

class FunctionLowCardinalityIndices: public IFunction
{
public:
    static constexpr auto name = "lowCardinalityIndices";
    static FunctionPtr create(ContextPtr) { return std::make_shared<FunctionLowCardinalityIndices>(); }

    String getName() const override { return name; }

    size_t getNumberOfArguments() const override { return 1; }

    bool useDefaultImplementationForNulls() const override { return false; }
    bool useDefaultImplementationForConstants() const override { return true; }
    bool useDefaultImplementationForLowCardinalityColumns() const override { return false; }

    DataTypePtr getReturnTypeImpl(const DataTypes & arguments) const override
    {
        const auto * type = typeid_cast<const DataTypeLowCardinality *>(arguments[0].get());
        if (!type)
            throw Exception("First first argument of function lowCardinalityIndexes must be ColumnLowCardinality, but got "
                            + arguments[0]->getName(), ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

        return std::make_shared<DataTypeUInt64>();
    }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr &, size_t /*input_rows_count*/) const override
    {
        const auto & arg = arguments[0];
        auto indexes_col = typeid_cast<const ColumnLowCardinality *>(arg.column.get())->getIndexesPtr();
        auto new_indexes_col = ColumnUInt64::create(indexes_col->size());
        auto & data = new_indexes_col->getData();
        for (size_t i = 0; i < data.size(); ++i)
            data[i] = indexes_col->getUInt(i);

        return new_indexes_col;
    }
};

}

REGISTER_FUNCTION(LowCardinalityIndices)
{
    factory.registerFunction<FunctionLowCardinalityIndices>();
}

}
