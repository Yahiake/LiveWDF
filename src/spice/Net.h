#pragma once

// The network layer, assembled from focused headers.
//
// The circuit is a flat list of two-terminal elements over *named* nodes.
// Ideal voltage sources are constraints, not unknowns, and are eliminated with a
// union-find over symbolic potentials. The compiled view turns that circuit into
// ports and affine offsets; the R-type transfer then solves the linear network
// once per topology or parameter change.
//
// The pieces are separate so a reader can find one job without reading the
// whole layer:
//
//     Circuit.h        element structs, `Circuit`, named-node list
//     Potentials.h     union-find with symbolic source potentials
//     Compiled.h       `Port`, `Compiled`, `compile`
//     Dense.h          setup-only dense solve
//     RootTransfer.h   element/root ports and the transfer matrices
//
// `Net.h` is the umbrella every existing caller includes. New code may include
// the narrow header it actually needs.

#include "spice/Circuit.h"
#include "spice/Potentials.h"
#include "spice/Compiled.h"
#include "spice/Dense.h"
#include "spice/RootTransfer.h"
