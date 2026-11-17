#pragma once

// The `.livewdf` format, assembled from focused headers.
//
//     Json.h           small JSON value/parser/serializer
//     Document.h       parts, wires, probes, and document questions
//     DocumentIO.cpp   save/load/JSON
//     DocumentCircuit.cpp  document -> wdf::Circuit
//     ControlGroups.h  pots/rheostats and SPDTs that move together
//
// Existing callers keep including `format/LiveWDFFile.h`; new code may include
// the narrow header it needs.

#include "format/ControlGroups.h"
#include "format/Document.h"
#include "format/Json.h"
