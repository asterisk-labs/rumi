const _INTERVAL = 0x01
const _INSTANT = 0x02

const _ISO_TIME = r"^(\d{4})-(\d{2})-(\d{2})(?:[T ](\d{2}):(\d{2})(?::(\d{2})(?:[.,](\d+))?)?(Z|z|[+-]\d{2}(?::?\d{2})?)?)?$"

function _parse_time(text::AbstractString)
    m = match(_ISO_TIME, text)
    m === nothing && throw(ArgumentError("$(repr(text)) is not an ISO date or datetime"))
    fields = [parse(Int, value) for value in m.captures[1:6] if value !== nothing]
    fraction = m.captures[7]
    fraction === nothing || all(==('0'), fraction) || throw(ArgumentError(
        "$(repr(text)) carries a fraction of a second; rumi records whole seconds"))
    value = try
        length(fields) == 3 ? Date(fields...) : DateTime(fields...)
    catch err
        err isa ArgumentError || rethrow()
        throw(ArgumentError("$(repr(text)) is not an ISO date or datetime"))
    end
    zone = m.captures[8]
    if zone !== nothing && uppercase(zone) != "Z"
        digits = filter(isdigit, zone)
        hours = parse(Int, digits[1:2])
        minutes = length(digits) > 2 ? parse(Int, digits[3:4]) : 0
        hours <= 23 && minutes <= 59 || throw(ArgumentError(
            "$(repr(text)) has an invalid UTC offset"))
        offset = Hour(hours) + Minute(minutes)
        # An offset names local time, and rumi records UTC.
        value = zone[1] == '-' ? value + offset : value - offset
    end
    return value
end

_seconds(value::Date) = Int64(Dates.value(value - Date(1970))) * 86400

function _seconds(value::DateTime)
    milliseconds = Dates.value(value - DateTime(1970))
    milliseconds % 1000 == 0 || throw(ArgumentError(
        "$value carries a fraction of a second; rumi records whole seconds"))
    return Int64(div(milliseconds, 1000))
end

_seconds(value::AbstractString) = _seconds(_parse_time(value))

_seconds(value) = throw(ArgumentError(
    "a time coordinate is a Date, a DateTime or an ISO string, got $(typeof(value))"))

# Return the time type and POSIX seconds the writer takes. A Date marks
# midnight UTC and a DateTime is read as UTC.
function _time_axis(time, steps::Integer)
    time === nothing && throw(ArgumentError(
        "every file labels its time steps; pass one date per step, or a " *
        "(start, end) pair for a step without a single instant"))
    time isa AbstractSet && throw(ArgumentError(
        "time must be ordered; a set cannot preserve time order"))
    time isa Tuple && throw(ArgumentError(
        "a tuple is one step's start and end, so it cannot be the list of " *
        "steps; wrap it as time=[(start, end)]"))
    time isa AbstractVector || throw(ArgumentError(
        "time is a vector with one entry per time step; wrap a single " *
        "coordinate as time=[$(repr(time))]"))
    isempty(time) && throw(ArgumentError(
        "a time axis covers every step, so $steps coordinates are needed; got none"))
    intervals = first(time) isa Tuple
    all(entry -> (entry isa Tuple) == intervals, time) || throw(ArgumentError(
        "a file records one kind of time step, so the vector is either all " *
        "instants or all (start, end) intervals, not a mix"))
    what = intervals ? "an interval" : "an instant"
    length(time) == steps || throw(ArgumentError(
        "$what axis needs one entry per time step, so $steps of them; got $(length(time))"))

    seconds = Int64[]
    for (step, entry) in enumerate(time)
        if intervals
            length(entry) == 2 || throw(ArgumentError(
                "time step $step is a pair of a start and an end, got $(length(entry)) values"))
            push!(seconds, _seconds(entry[1]), _seconds(entry[2]))
        else
            push!(seconds, _seconds(entry))
        end
    end
    return intervals ? _INTERVAL : _INSTANT, seconds
end
