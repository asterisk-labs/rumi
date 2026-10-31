module Rumi

using Artifacts
using Dates
using Downloads
using LazyArtifacts
using Libdl

import Base: close, isopen, show

export Dataset, Metadata, RumiError, info, read_many

include("lib.jl")
include("types.jl")
include("api.jl")

end
