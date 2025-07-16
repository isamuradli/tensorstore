#include <rocksdb/db.h>

#include <iostream>

#include "tensorstore/array.h"
#include "tensorstore/open.h"
#include "tensorstore/open_mode.h"
#include "tensorstore/tensorstore.h"
#include "tensorstore/util/status.h"
using namespace std;
int main() {
  // Define the shape and data type of the tensor
  std::vector<int> shape = {4, 4};
  tensorstore::DataType dtype = tensorstore::dtype_v<int>;

  // Define the TensorStore spec for Zarr format
  std::cout << "Custom Start From JSON" << endl;

  auto spec = tensorstore::Spec::FromJson({{"driver", "zarr"},
                                           {"kvstore",
                                            {{"driver", "rocksdb"},
                                             {"path", "output.zarr"},
                                             {"database_name", "mydb"},
                                             }},
                                           {"metadata",
                                            {
                                                {"dtype", "<i4"},
                                                {"shape", shape},
                                                {"chunks", {1,1}},
                                            }}})
                  .value();

  std::cout << "\ncustom_example.cc - > tensorstore::Open( ... )\n" << endl;
  // Open the TensorStore
  auto store = tensorstore::Open(spec, tensorstore::OpenMode::open_or_create,
                                 tensorstore::ReadWriteMode::read_write)
                   .value();
  std::cout << "\nTensorStore opened successfully.\n" << std::endl;

  // return 0;

  auto array = tensorstore::MakeArray(
      {{1, 2, 3, 1}, {6, 7, 8, 2}, {11, 12, 15, 4}, {1, 1, 1, 1}});

  tensorstore::WriteOptions write_options;
  // Write the tensor data to the TensorStore
  std::cout << "Custom Start Tensor:WRITE" << endl;

  auto write_result =
      tensorstore::Write(array, store, std::move(write_options)).result();
  if (!write_result.ok()) {
    std::cerr << "Error writing tensor: " << write_result.status() << std::endl;
    return 1;
  }
  std::cout << "\n\n$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$"
               "$$$$\n\n"
            << std::endl;

  auto read_result = tensorstore::Read(store).result();

  if (!read_result.ok()) {
    std::cerr << "Error reading tensor: " << read_result.status() << std::endl;
    return 1;
  }

  // Retrieve the tensor array from the result.
  auto readarray = std::move(read_result).value();

  std::cout << "Tensor read successfully from output.zarr:" << std::endl;
  std::cout << readarray << std::endl;

  std::cout << "Tensor written successfully to tensor.zarr" << std::endl;
  return 0;
}
