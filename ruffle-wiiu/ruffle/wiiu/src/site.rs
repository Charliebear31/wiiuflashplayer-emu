//! Games saved with their website: the page that embeds a game gives it settings (FlashVars).

/// The settings the page gives the movie: from the page's scripts and tags, and the query on its link to the SWF.
pub fn page_settings(page_path: &str, movie_url: &str) -> Vec<(String, String)> {
    let Ok(bytes) = std::fs::read(page_path) else { return Vec::new() };
    let text = String::from_utf8_lossy(&bytes);
    let mut out = Vec::new();
    let movie = movie_url.rsplit('/').next().unwrap_or("").replace("%20", " ");
    query_after_swf(&text, &movie, &mut out);
    flashvars(&text, &mut out);
    add_variables(&text, &mut out);
    out
}

/// `"flash/game.swf?a=1&b=2"`: an expression may follow the closing quote, but the text inside it is what counts.
fn query_after_swf(text: &str, movie: &str, out: &mut Vec<(String, String)>) {
    let lower = text.to_ascii_lowercase();
    let wanted = format!("{}?", movie.to_ascii_lowercase());
    let Some(at) = lower.find(&wanted) else { return };
    let rest = &text[at + wanted.len()..];
    let end = rest.find(['"', '\'', '\n']).unwrap_or(rest.len());
    pairs(&rest[..end], out);
}

fn flashvars(text: &str, out: &mut Vec<(String, String)>) {
    let lower = text.to_ascii_lowercase();
    let mut from = 0;
    while let Some(found) = lower[from..].find("flashvars") {
        let after = from + found + "flashvars".len();
        from = after;
        let rest = text[after..].trim_start();
        if let Some(field) = rest.strip_prefix('.') {
            // flashvars.name = "value";
            let Some((name, value)) = field.split_once('=') else { continue };
            let name = name.trim();
            if is_identifier(name)
                && let Some((value, _)) = quoted(value.trim_start())
            {
                out.push((name.to_string(), value.to_string()));
            }
        } else if let Some(value) = rest.strip_prefix('=') {
            let value = value.trim_start();
            if let Some(object) = value.strip_prefix('{') {
                object_fields(object, out);
            } else if let Some((value, _)) = quoted(value) {
                // flashvars="a=1&b=2"
                pairs(value, out);
            }
        } else if let Some(attr) = rest.strip_prefix(['"', '\'']) {
            // name="flashvars" value="a=1&b=2"
            let Some(value) = attr.trim_start().strip_prefix("value").or_else(|| attr.trim_start().strip_prefix("VALUE")) else { continue };
            let Some(value) = value.trim_start().strip_prefix('=') else { continue };
            if let Some((value, _)) = quoted(value.trim_start()) {
                pairs(value, out);
            }
        }
    }
}

/// `{a: "1", "b": 2}`, after its opening brace. Values that aren't plain text or numbers can't be worked out here.
fn object_fields(mut text: &str, out: &mut Vec<(String, String)>) {
    loop {
        text = text.trim_start_matches(|c: char| c.is_whitespace() || c == ',');
        if text.is_empty() || text.starts_with('}') {
            return;
        }
        let (key, rest) = match quoted(text) {
            Some(found) => found,
            None => {
                let end = text.find(|c: char| !is_identifier_char(c)).unwrap_or(text.len());
                if end == 0 {
                    return;
                }
                (&text[..end], &text[end..])
            }
        };
        let Some(rest) = rest.trim_start().strip_prefix(':') else { return };
        let rest = rest.trim_start();
        if let Some((value, after)) = quoted(rest) {
            out.push((key.to_string(), value.to_string()));
            text = after;
        } else {
            let end = rest.find([',', '}', '\n']).unwrap_or(rest.len());
            let value = rest[..end].trim();
            if value.parse::<f64>().is_ok() || value == "true" || value == "false" {
                out.push((key.to_string(), value.to_string()));
            }
            text = &rest[end..];
        }
    }
}

/// SWFObject 1: `so.addVariable("name", "value");`
fn add_variables(text: &str, out: &mut Vec<(String, String)>) {
    for part in text.split("addVariable(").skip(1) {
        let Some((name, rest)) = quoted(part.trim_start()) else { continue };
        let Some(rest) = rest.trim_start().strip_prefix(',') else { continue };
        if let Some((value, _)) = quoted(rest.trim_start()) {
            out.push((name.to_string(), value.to_string()));
        }
    }
}

/// `"text" and the rest` -> ("text", " and the rest")
fn quoted(text: &str) -> Option<(&str, &str)> {
    let quote = text.chars().next().filter(|c| *c == '"' || *c == '\'')?;
    let body = &text[1..];
    let end = body.find(quote)?;
    Some((&body[..end], &body[end + 1..]))
}

fn pairs(query: &str, out: &mut Vec<(String, String)>) {
    for pair in query.replace("&amp;", "&").split('&') {
        if let Some((name, value)) = pair.split_once('=')
            && !name.is_empty()
        {
            out.push((name.to_string(), crate::navigator::percent_decode(value)));
        }
    }
}

fn is_identifier_char(c: char) -> bool {
    c.is_ascii_alphanumeric() || c == '_' || c == '$'
}

fn is_identifier(text: &str) -> bool {
    !text.is_empty() && text.chars().all(is_identifier_char)
}
