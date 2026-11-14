module Rumi

using Artifacts
using Dates
using Downloads
using LazyArtifacts
using Libdl

import Base: close, isopen, show

export Dataset, FrameTable, Metadata, RumiError, info, read_many

include("lib.jl")
include("types.jl")
include("api.jl")
include("frames.jl")
include("time.jl")
include("write.jl")

end
