/**
 * This file is part of the "libfnord" project
 *   Copyright (c) 2015 Paul Asmuth
 *
 * FnordMetric is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License v3.0. You should have received a
 * copy of the GNU General Public License along with this program. If not, see
 * <http://www.gnu.org/licenses/>.
 */
#include "fnord/csv/BinaryCSVInputStream.h"

namespace fnord {

BinaryCSVInputStream::BinaryCSVInputStream(
    std::unique_ptr<RewindableInputStream>&& input_stream) :
    input_(std::move(input_stream)) {}

bool BinaryCSVInputStream::readNextRow(std::vector<std::string>* target) {
  if (input_->eof()) {
    return false;
  }

  auto row_size = input_->readVarUInt();

  for (size_t i = 0; i < row_size; ++i) {
    target->emplace_back(input_->readLenencString());
  }

  return true;
}

bool BinaryCSVInputStream::skipNextRow() {
  Vector<String> devnull;
  return readNextRow(&devnull);
}

void BinaryCSVInputStream::rewind() {
  input_->rewind();
}

const RewindableInputStream& BinaryCSVInputStream::getInputStream() const {
  return *input_;
}

}
