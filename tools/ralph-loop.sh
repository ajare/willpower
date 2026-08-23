#!/usr/bin/env bash
# Runs ready GitHub tickets through pi or Claude Code until no dependency-free
# work remains. Requires Bash 4+, gh, git, jq, curl, and the selected agent.

set -uo pipefail

usage() {
    cat <<'EOF'
Usage: ralph-loop.sh --agent pi|claude [options]

Options:
  --agent NAME                    Coding agent (required: pi or claude)
  --model NAME                    Model (default: agent's larger model)
  --effort LEVEL                  Thinking effort (default: medium)
  --adaptive-model-and-effort     Select model/effort from difficulty label
  --repo OWNER/NAME               Repository (default: current repository)
  --ready-label LABEL             Ready label (default: ready-for-agent)
  --initial-retry-seconds N       Initial provider retry delay (default: 30)
  --max-retry-seconds N           Maximum provider retry delay (default: 900)
  --usage-poll-seconds N          Usage-limit retry delay (default: 600)
  --once                          Process at most one ticket
  --dry-run                       Show the next ticket without claiming it
  --quiet                         Suppress routine output
  --verbose                       Enable loop and agent diagnostics
  -h, --help                      Show this help

Priority labels are P0, P1, P2, and so on; priority/P0 is also accepted.
Adaptive difficulty labels are trivial, easy, medium, and hard; labels may use
the difficulty/ namespace. Difficulty only affects Claude, where adaptive
selection uses Sonnet/Opus. Pi always uses its default model and effort.
EOF
}

agent=""
model=""
effort="medium"
adaptive=false
repo=""
ready_label="ready-for-agent"
initial_retry_seconds=30
max_retry_seconds=900
usage_poll_seconds=600
once=false
dry_run=false
quiet=false
verbose=false

need_value() {
    if (($# < 2)); then
        printf 'Missing value for %s\n' "$1" >&2
        exit 2
    fi
}

while (($#)); do
    case "$1" in
        --agent) need_value "$@"; agent=$2; shift 2 ;;
        --model) need_value "$@"; model=$2; shift 2 ;;
        --effort) need_value "$@"; effort=$2; shift 2 ;;
        --adaptive-model-and-effort) adaptive=true; shift ;;
        --repo) need_value "$@"; repo=$2; shift 2 ;;
        --ready-label) need_value "$@"; ready_label=$2; shift 2 ;;
        --initial-retry-seconds) need_value "$@"; initial_retry_seconds=$2; shift 2 ;;
        --max-retry-seconds) need_value "$@"; max_retry_seconds=$2; shift 2 ;;
        --usage-poll-seconds) need_value "$@"; usage_poll_seconds=$2; shift 2 ;;
        --once) once=true; shift ;;
        --dry-run) dry_run=true; shift ;;
        --quiet) quiet=true; shift ;;
        --verbose) verbose=true; shift ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'Unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done

status() { $quiet || printf '%s\n' "$*"; }
verbose_status() { $verbose && printf '%s\n' "$*"; return 0; }
die() { printf 'Error: %s\n' "$*" >&2; exit 1; }
warn() { printf 'Warning: %s\n' "$*" >&2; }

for command_name in gh git jq curl; do
    command -v "$command_name" >/dev/null 2>&1 || die "$command_name is required but was not found on PATH."
done
[[ $agent == pi || $agent == claude ]] || die "--agent must be pi or claude."
command -v "$agent" >/dev/null 2>&1 || die "$agent is required but was not found on PATH."
[[ $initial_retry_seconds =~ ^[0-9]+$ && $initial_retry_seconds -gt 0 ]] || die "Initial retry interval must be positive."
[[ $max_retry_seconds =~ ^[0-9]+$ && $max_retry_seconds -ge $initial_retry_seconds ]] || die "Maximum retry interval must be at least the initial interval."
[[ $usage_poll_seconds =~ ^[0-9]+$ && $usage_poll_seconds -gt 0 ]] || die "Usage poll interval must be positive."
$quiet && $verbose && die "--quiet and --verbose cannot be used together."

case "$effort" in off|minimal|low|medium|high|xhigh|max) ;; *) die "Unsupported effort '$effort'." ;; esac
if [[ $agent == claude && ($effort == off || $effort == minimal) ]]; then
    die "Effort '$effort' is not supported by claude. Use low, medium, high, xhigh, or max."
fi

get_model_pair() {
    if [[ $1 == pi ]]; then
        smaller_model="llama-server/qwen-3.8-27b"
        larger_model="llama-server/qwen-3.8-27b"
    else
        smaller_model="sonnet"
        larger_model="opus"
    fi
}
get_model_pair "$agent"
[[ -n $model ]] || model=$larger_model

repo_root=$(git rev-parse --show-toplevel 2>/dev/null) || die "Run this script from inside a Git repository."
cd "$repo_root" || exit 1
[[ -z $(git status --porcelain --untracked-files=no) ]] || die "The tracked worktree is not clean. Commit or restore tracked changes before starting the loop."

if [[ -z $repo ]]; then
    repo=$(gh repo view --json nameWithOwner --jq .nameWithOwner) || die "Could not infer the GitHub repository."
fi
current_user=$(gh api user --jq .login) || die "Could not determine the current GitHub user."
log_directory="${TMPDIR:-/tmp}/${agent}-ralph-loop"
mkdir -p "$log_directory"

priority_of_labels() {
    local labels_json=$1 name number rank=100
    while IFS= read -r name; do
        name=$(printf '%s' "$name" | tr '[:upper:]' '[:lower:]' | xargs)
        if [[ $name =~ ^(priority[/\:][[:space:]]*)?p([0-9]+)$ ]]; then
            number=${BASH_REMATCH[2]}
            ((number < rank)) && rank=$number
        fi
    done < <(jq -r '.[].name' <<<"$labels_json")
    printf '%s' "$rank"
}

get_next_ticket() {
    local issues issue number body assignees assigned detail blocked priority resume
    local parent_numbers=" " candidates=""
    issues=$(gh issue list --repo "$repo" --state open --label "$ready_label" --limit 100 \
        --json number,title,body,labels,assignees,url) || die "Could not list issues."
    [[ $(jq 'length' <<<"$issues") -gt 0 ]] || return 1

    while IFS= read -r number; do
        parent_numbers+="$number "
    done < <(jq -r '.[] | (.body // "") |
        capture("(?im)^## Parent\\s*\\r?\\n+\\s*#(?<number>[0-9]+)")?.number // empty' <<<"$issues")

    while IFS= read -r issue; do
        number=$(jq -r .number <<<"$issue")
        [[ $parent_numbers == *" $number "* ]] && continue
        assignees=$(jq '.assignees' <<<"$issue")
        assigned=$(jq --arg user "$current_user" 'any(.[]; .login == $user)' <<<"$assignees")
        if [[ $(jq 'length' <<<"$assignees") -gt 0 && $assigned != true ]]; then
            continue
        fi
        detail=$(gh api "repos/$repo/issues/$number") || die "Could not inspect issue #$number."
        blocked=$(jq -r '.issue_dependencies_summary.blocked_by // 0' <<<"$detail")
        ((blocked > 0)) && continue
        $assigned && resume=0 || resume=1
        priority=$(priority_of_labels "$(jq '.labels' <<<"$issue")")
        candidates+=$(printf '%d\t%d\t%010d\t%s' "$resume" "$priority" "$number" "$(jq -c . <<<"$issue")")$'\n'
    done < <(jq -c '.[]' <<<"$issues")

    [[ -n $candidates ]] || return 1
    # printf (not a herestring) so the trailing newline already in $candidates does
    # not become a spurious empty line that sorts first under -n.
    printf '%s' "$candidates" | sort -n -k1,1 -k2,2 -k3,3 | head -n1 | cut -f4-
}

select_adaptive() {
    local labels_json=$1 label value values=" " count=0
    while IFS= read -r label; do
        label=$(printf '%s' "$label" | tr '[:upper:]' '[:lower:]' | xargs)
        if [[ $label =~ ^(difficulty[/\:][[:space:]]*)?(trivial|easy|medium|hard)$ ]]; then
            value=${BASH_REMATCH[2]}
            if [[ $values != *" $value "* ]]; then
                values+="$value "
                difficulty=$value
                ((++count))
            fi
        fi
    done < <(jq -r '.[].name' <<<"$labels_json")
    ((count > 0)) || die "Adaptive model and effort requires difficulty/trivial, difficulty/easy, difficulty/medium, or difficulty/hard."
    ((count == 1)) || die "Adaptive model and effort found conflicting difficulty labels:${values}."

    get_model_pair "$agent"
    case "$difficulty" in
        trivial|easy|medium) ticket_model=$smaller_model; ticket_effort=medium ;;
        hard) ticket_model=$larger_model; ticket_effort=medium ;;
    esac
}

get_ticket_prompt() {
    local number=$1 issue comments comment_text
    issue=$(gh issue view "$number" --repo "$repo" --json number,title,body,comments,url) || die "Could not read issue #$number."
    comments=$(jq -r '[.comments[].body] | join("\n\n---\n\n")' <<<"$issue")
    [[ -n $comments ]] && comment_text=$comments || comment_text="(No comments.)"
    cat <<EOF
Implement GitHub ticket #$(jq -r .number <<<"$issue"): $(jq -r .title <<<"$issue")
$(jq -r .url <<<"$issue")

You are running non-interactively. Work autonomously through implementation; do not stop at a plan and do not ask the user questions. Read and follow the repository instructions and domain documentation. Inspect the current worktree first because this may be a retry after a provider failure.

Only implement this ticket, not its parent or blocked follow-up tickets. Use the ticket's acceptance criteria as the contract. Run focused tests while developing, then the relevant builds, formatting checks, and tests before completion. Preserve unrelated and pre-existing untracked files.

When the ticket is fully implemented and verified:
1. Commit all tracked changes on the current branch with a message referencing #$(jq -r .number <<<"$issue").
2. Close #$(jq -r .number <<<"$issue") with a concise comment containing the commit hash and validation performed.
3. Finish with a concise implementation summary.

If implementation cannot be completed for a code, test, or specification reason, leave the issue open, do not commit partial work merely to satisfy this prompt, and explain the blocker in your final response.

## Ticket body

$(jq -r '.body // ""' <<<"$issue")

## Ticket comments

$comment_text
EOF
}

ticket_complete() {
    local number=$1 starting_head=$2 state current_head
    state=$(gh issue view "$number" --repo "$repo" --json state --jq .state) || return 1
    current_head=$(git rev-parse HEAD) || return 1
    [[ $state == CLOSED && $current_head != "$starting_head" && -z $(git status --porcelain --untracked-files=no) ]]
}

is_usage_limit_error() {
    grep -Eqi 'usage limit|usage_limit_reached|usage cap|quota exceeded|insufficient_quota|out of credits|credit balance|billing limit|subscription limit|weekly limit|monthly limit|weighted tokens|token limit.*reset|rate limit.*reset|limit resets? at' <<<"$1"
}

is_server_error() {
    grep -Eqi 'HTTP[[:space:]]*(408|409|425|429|5[0-9][0-9])|status[[:space:]]*(408|409|425|429|5[0-9][0-9])|server error|internal server error|service unavailable|bad gateway|gateway timeout|overloaded|temporarily unavailable|request timeout|timed out|ECONNRESET|ECONNREFUSED|ENETUNREACH|EAI_AGAIN|socket hang up|connection reset|connection closed|fetch failed|network error|server_error|stream.*(closed|terminated)' <<<"$1"
}

iso_now() { date --iso-8601=seconds; }
elapsed() {
    local seconds=$1
    printf '%02d:%02d:%02d' "$((seconds / 3600))" "$(((seconds % 3600) / 60))" "$((seconds % 60))"
}

# Sets usage_json to normalized transcript totals, or leaves it empty.
get_pi_session_usage() {
    local directory=$1
    usage_json=$(find "$directory" -type f -name '*.jsonl' -print0 2>/dev/null | xargs -0 -r jq -sc '
      [ .[] | select(.type == "message" and .message.role == "assistant" and .message.usage != null) | .message ] as $m |
      if ($m|length)==0 then empty else {
        Provider: ($m|map(.provider // "")|last), Model: ($m|map(.model // "")|last),
        Input: ($m|map(.usage.input // 0)|add), Output: ($m|map(.usage.output // 0)|add),
        CacheRead: ($m|map(.usage.cacheRead // 0)|add), CacheWrite: ($m|map(.usage.cacheWrite // 0)|add),
        Reasoning: ($m|map(.usage.reasoning // 0)|add), TotalTokens: ($m|map(.usage.totalTokens // 0)|add),
        Cost: ($m|map(.usage.cost.total // 0)|add), CostKnown: true } end' 2>/dev/null || true)
}

get_claude_session_usage() {
    local config_directory=${CLAUDE_CONFIG_DIR:-$HOME/.claude} id file files=()
    for id in "$@"; do
        while IFS= read -r -d '' file; do files+=("$file"); done < <(find "$config_directory/projects" -type f -name "$id.jsonl" -print0 2>/dev/null)
    done
    ((${#files[@]})) || { usage_json=""; return; }
    usage_json=$(jq -sc '
      [ .[] | select(.type == "assistant" and .message.usage != null) | .message ] as $m |
      if ($m|length)==0 then empty else {
        Provider:"anthropic", Model:($m|map(.model // "")|last),
        Input:($m|map(.usage.input_tokens // 0)|add), Output:($m|map(.usage.output_tokens // 0)|add),
        CacheRead:($m|map(.usage.cache_read_input_tokens // 0)|add), CacheWrite:($m|map(.usage.cache_creation_input_tokens // 0)|add),
        Reasoning:($m|map(.usage.output_tokens_details.thinking_tokens // 0)|add),
        TotalTokens:($m|map((.usage.input_tokens // 0)+(.usage.output_tokens // 0)+(.usage.cache_read_input_tokens // 0)+(.usage.cache_creation_input_tokens // 0))|add),
        Cost:0, CostKnown:false } end' "${files[@]}" 2>/dev/null || true)
}

show_ticket_summary() {
    local number=$1 started=$2 ended=$3 duration=$4
    printf 'Ticket #%s summary\n  Started: %s\n  Ended: %s\n  Duration: %s\n' "$number" "$started" "$ended" "$(elapsed "$duration")"
    if [[ -z $usage_json ]]; then
        printf '  Total tokens spent: unavailable\n'
        return
    fi
    local total input output reasoning cache_read cache_write cost known
    read -r total input output reasoning cache_read cache_write cost known < <(jq -r '[.TotalTokens,.Input,.Output,.Reasoning,.CacheRead,.CacheWrite,.Cost,.CostKnown]|@tsv' <<<"$usage_json")
    if [[ $known == true ]]; then
        printf '  Total tokens spent: %s (input %s, output %s, reasoning %s, cache read %s, cache write %s); cost $%.4f\n' "$total" "$input" "$output" "$reasoning" "$cache_read" "$cache_write" "$cost"
    else
        printf '  Total tokens spent: %s (input %s, output %s, reasoning %s, cache read %s, cache write %s); cost not reported by this agent\n' "$total" "$input" "$output" "$reasoning" "$cache_read" "$cache_write"
    fi
}

show_usage_window() {
    local name=$1 window=$2 used reset
    if [[ $window == null || -z $window ]]; then printf '  %s: not reported\n' "$name"; return; fi
    used=$(jq -r '.used_percent // .utilization // empty' <<<"$window")
    [[ -n $used ]] || { printf '  %s: usage not reported\n' "$name"; return; }
    printf '  %s: %.1f%% used, %.1f%% remaining\n' "$name" "$used" "$(awk -v u="$used" 'BEGIN { r=100-u; print r<0?0:r }')"
}

show_provider_usage() {
    local provider provider_model auth_file token account response primary secondary
    provider=$(jq -r '.Provider // empty' <<<"$usage_json")
    provider_model=$(jq -r '.Model // empty' <<<"$usage_json")
    [[ -n $provider ]] || provider=${model%%/*}
    printf 'Provider usage (%s/%s)\n' "$provider" "$provider_model"
    if [[ $provider == anthropic ]]; then
        auth_file="${CLAUDE_CONFIG_DIR:-$HOME/.claude}/.credentials.json"
        token=$(jq -r '.claudeAiOauth.accessToken // empty' "$auth_file" 2>/dev/null) || true
        [[ -n $token ]] || { warn "Could not read Anthropic OAuth credentials."; return; }
        response=$(curl -fsS -H "Authorization: Bearer $token" -H 'anthropic-beta: oauth-2025-04-20' https://api.anthropic.com/api/oauth/usage) || { warn "Could not read provider usage."; return; }
        show_usage_window "Current window" "$(jq -c '.five_hour // null' <<<"$response")"
        show_usage_window "Weekly" "$(jq -c '.seven_day // null' <<<"$response")"
    elif [[ $provider == openai-codex ]]; then
        auth_file="${PI_CODING_AGENT_DIR:-$HOME/.pi/agent}/auth.json"
        token=$(jq -r '.["openai-codex"].access // empty' "$auth_file" 2>/dev/null) || true
        account=$(jq -r '.["openai-codex"].accountId // empty' "$auth_file" 2>/dev/null) || true
        [[ -n $token ]] || { warn "Could not read OpenAI OAuth credentials."; return; }
        response=$(curl -fsS -H "Authorization: Bearer $token" -H "ChatGPT-Account-Id: $account" https://chatgpt.com/backend-api/wham/usage) || { warn "Could not read provider usage."; return; }
        primary=$(jq -c '.rate_limit.primary_window // null' <<<"$response")
        secondary=$(jq -c '.rate_limit.secondary_window // null' <<<"$response")
        if [[ $secondary == null && $(jq -r '.limit_window_seconds // 0' <<<"$primary") -ge 518400 ]]; then
            secondary=$primary; primary=null
        fi
        show_usage_window "Current window" "$primary"
        show_usage_window "Weekly" "$secondary"
    else
        printf '  Current window: not available from this provider\n  Weekly: not available from this provider\n'
    fi
}

while true; do
    if ! ticket=$(get_next_ticket); then
        status "No unblocked, unclaimed '$ready_label' tickets are available."
        break
    fi
    number=$(jq -r .number <<<"$ticket")
    status "Selected #$number: $(jq -r .title <<<"$ticket")"

    ticket_model=$model
    ticket_effort=$effort
    selection_source="command line/default"
    if $adaptive; then
        if [[ $agent == pi ]]; then
            # Pi's defaults are used for every ticket; difficulty is ignored.
            get_model_pair "$agent"
            ticket_model=$larger_model
            ticket_effort=medium
            selection_source="pi defaults"
        else
            select_adaptive "$(jq '.labels' <<<"$ticket")"
            selection_source="adaptive difficulty:$difficulty"
        fi
    fi
    printf 'Ticket #%s model: %s; effort: %s (%s).\n' "$number" "$ticket_model" "$ticket_effort" "$selection_source"
    if $dry_run; then
        status "Dry run: would start $agent with model '$ticket_model' and effort '$ticket_effort'."
        break
    fi

    if [[ $(jq '.assignees | length' <<<"$ticket") -eq 0 ]]; then
        gh issue edit "$number" --repo "$repo" --add-assignee @me >/dev/null || die "Could not claim #$number."
        status "Claimed #$number as $current_user."
    fi

    ticket_started=$(iso_now); ticket_started_epoch=$(date +%s)
    timestamp=$(date +%Y%m%d-%H%M%S)
    ticket_session_directory="$log_directory/issue-$number-$timestamp-sessions"
    session_ids=()
    [[ $agent != pi ]] || mkdir -p "$ticket_session_directory"
    status "Ticket #$number started at $ticket_started."
    starting_head=$(git rev-parse HEAD)
    prompt=$(get_ticket_prompt "$number")
    retry_interval=$initial_retry_seconds
    attempt=0

    while true; do
        ((++attempt))
        attempt_timestamp=$(date +%Y%m%d-%H%M%S)
        log_path="$log_directory/issue-$number-$attempt_timestamp-attempt-$attempt.log"
        status "Starting $agent for #$number (attempt $attempt). Log: $log_path"
        agent_args=()
        if [[ $agent == pi ]]; then
            session_directory="$ticket_session_directory/attempt-$attempt"
            mkdir -p "$session_directory"
            verbose_status "Session directory: $session_directory"
            agent_args=(--print --approve --model "$ticket_model" --thinking "$ticket_effort" --name "ralph-$number" --session-dir "$session_directory")
        else
            session_id=$(cat /proc/sys/kernel/random/uuid 2>/dev/null || uuidgen)
            session_ids+=("$session_id")
            verbose_status "Session id: $session_id"
            agent_args=(--print --dangerously-skip-permissions --model "$ticket_model" --effort "$ticket_effort" --session-id "$session_id")
        fi
        $verbose && agent_args+=(--verbose)
        agent_args+=("$prompt")

        set +e
        if $quiet; then
            "$agent" "${agent_args[@]}" 2>&1 | tee -a "$log_path" >/dev/null
        else
            "$agent" "${agent_args[@]}" 2>&1 | tee -a "$log_path"
        fi
        agent_exit=${PIPESTATUS[0]}
        output_text=$(<"$log_path")

        if ticket_complete "$number" "$starting_head"; then
            status "Ticket #$number completed successfully."
            break
        fi
        if ((agent_exit == 0)); then
            die "$agent exited successfully, but #$number was not closed with a new commit and clean tracked worktree. Inspect $log_path."
        elif is_usage_limit_error "$output_text"; then
            warn "Usage limit detected for #$number. Retrying in $usage_poll_seconds seconds."
            sleep "$usage_poll_seconds"
        elif is_server_error "$output_text"; then
            warn "Server/API failure detected for #$number. Retrying in $retry_interval seconds."
            sleep "$retry_interval"
            retry_interval=$((retry_interval * 2))
            ((retry_interval <= max_retry_seconds)) || retry_interval=$max_retry_seconds
        else
            die "$agent failed for a non-retryable implementation reason on #$number. The issue remains assigned and open. Inspect $log_path."
        fi
    done

    ticket_ended=$(iso_now); ticket_ended_epoch=$(date +%s)
    usage_json=""
    if [[ $agent == pi ]]; then
        get_pi_session_usage "$ticket_session_directory"
        usage_source=$ticket_session_directory
    else
        get_claude_session_usage "${session_ids[@]}"
        usage_source="Claude Code sessions ${session_ids[*]}"
    fi
    show_ticket_summary "$number" "$ticket_started" "$ticket_ended" "$((ticket_ended_epoch-ticket_started_epoch))"
    if [[ -z $usage_json ]]; then
        warn "Could not read current-ticket usage from $usage_source."
    else
        show_provider_usage
    fi
    $once && break
done
