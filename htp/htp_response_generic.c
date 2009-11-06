
#include "htp.h"

/**
 * Generic response line parser.
 * 
 * @param connp
 * @return HTP status
 */
int htp_parse_response_line_generic(htp_connp_t *connp) {
    htp_tx_t *tx = connp->out_tx;
    unsigned char *data = bstr_ptr(tx->response_line);
    size_t len = bstr_len(tx->response_line);
    int pos = 0;   

    // The request method starts at the beginning of the
    // line and ends with the first whitespace character.
    while ((pos < len) && (!htp_is_space(data[pos]))) {
        pos++;
    }

    tx->response_protocol = bstr_memdup(data, pos);
    tx->response_protocol_number = htp_parse_protocol(tx->response_protocol);

    // Ignore whitespace after response protocol
    while ((pos < len) && (isspace(data[pos]))) {
        pos++;
    }

    size_t start = pos;

    // Find the next whitespace character
    while ((pos < len) && (!htp_is_space(data[pos]))) {
        pos++;
    }

    tx->response_status = bstr_memdup(data + start, pos - start);
    tx->response_status_number = htp_parse_status(tx->response_status);

    // Ignore whitespace that follows
    while ((pos < len) && (isspace(data[pos]))) {
        pos++;
    }

    tx->response_message = bstr_memdup(data + pos, len - pos);
    
    return HTP_OK;
}

/**
 * Generic response header parser.
 * 
 * @param connp
 * @param h
 * @param data
 * @param len
 * @return HTP status
 */
int htp_parse_response_header_generic(htp_connp_t *connp, htp_header_t *h, char *data, size_t len) {
    size_t name_start, name_end;
    size_t value_start, value_end;

    name_start = 0;

    // Look for the colon
    int colon_pos = 0;
    while ((colon_pos < len) && (data[colon_pos] != ':')) colon_pos++;

    if (colon_pos == len) {
        // Missing colon
        h->flags |= HTP_FIELD_UNPARSEABLE;

        if (!(connp->out_tx->flags & HTP_FIELD_UNPARSEABLE)) {
            connp->out_tx->flags |= HTP_FIELD_UNPARSEABLE;
            // Only log once per transaction
            htp_log(connp, LOG_MARK, LOG_ERROR, 0, "Request field invalid: colon missing");
        }

        return -1;
    }

    if (colon_pos == 0) {
        // Empty header name
        h->flags |= HTP_FIELD_INVALID;

        if (!(connp->out_tx->flags & HTP_FIELD_INVALID)) {
            connp->out_tx->flags |= HTP_FIELD_INVALID;
            // Only log once per transaction
            htp_log(connp, LOG_MARK, LOG_WARNING, 0, "Request field invalid: empty name");
        }
    }

    name_end = colon_pos;

    // Ignore LWS after field-name
    size_t prev = name_end - 1;
    while ((prev > name_start) && (htp_is_lws(data[prev]))) {
        prev--;
        name_end--;

        h->flags |= HTP_FIELD_INVALID;

        if (!(connp->out_tx->flags & HTP_FIELD_INVALID)) {
            connp->out_tx->flags |= HTP_FIELD_INVALID;
            htp_log(connp, LOG_MARK, LOG_WARNING, 0, "Request field invalid: LWS after name");
        }
    }

    // Value

    value_start = colon_pos;

    // Go over the colon
    if (value_start < len) {
        value_start++;
    }

    // Ignore LWS before field-content
    while ((value_start < len) && (htp_is_lws(data[value_start]))) {
        value_start++;
    }

    // Look for the end of field-content
    value_end = value_start;
    while (value_end < len) value_end++;

    // Ignore LWS after field-content
    prev = value_end - 1;
    while ((prev > value_start) && (htp_is_lws(data[prev]))) {
        prev--;
        value_end--;
    }

    // Check that the header name is a token
    int i = name_start;
    while (i < name_end) {
        if (!htp_is_token(data[i])) {
            h->flags |= HTP_FIELD_INVALID;

            if (!(connp->out_tx->flags & HTP_FIELD_INVALID)) {
                connp->out_tx->flags |= HTP_FIELD_INVALID;
                htp_log(connp, LOG_MARK, LOG_WARNING, 0, "Request header name is not a token");
            }

            break;
        }

        i++;
    }

    // Now extract the name and the value
    h->name = bstr_memdup(data + name_start, name_end - name_start);
    h->value = bstr_memdup(data + value_start, value_end - value_start);

    return HTP_OK;
}

/**
 * Generic response header line(s) processor, which assembles folded lines
 * into a single buffer before invoking the parsing function.
 * 
 * @param connp
 * @return HTP status
 */
int htp_process_response_header_generic(htp_connp_t *connp) {
    bstr *tempstr = NULL;
    char *data = NULL;
    size_t len = 0;

    // Parse header
    htp_header_t *h = calloc(1, sizeof (htp_header_t));
    if (h == NULL) {
        // TODO
        return HTP_ERROR;
    }   

    // Ensure we have the necessary header data in a single buffer
    if (connp->out_header_line_index + 1 == connp->out_header_line_counter) {
        // Single line
        htp_header_line_t *hl = list_get(connp->out_tx->response_header_lines,
            connp->out_header_line_index);
        if (hl == NULL) {
            // Internal error
            // TODO
            free(h);
            return HTP_ERROR;
        }

        data = bstr_ptr(hl->line);
        len = bstr_len(hl->line);
        hl->header = h;
    } else {
        // Multiple lines (folded)
        int i = 0;

        for (i = connp->out_header_line_index; i < connp->out_header_line_counter; i++) {
            htp_header_line_t *hl = list_get(connp->out_tx->response_header_lines, i);
            len += bstr_len(hl->line);
        }

        tempstr = bstr_alloc(len);
        if (tempstr == NULL) {
            // TODO
            free(h);
            return HTP_ERROR;
        }

        for (i = connp->out_header_line_index; i < connp->out_header_line_counter; i++) {
            htp_header_line_t *hl = list_get(connp->out_tx->response_header_lines, i);
            bstr_add_str(tempstr, hl->line);
            hl->header = h;
        }

        data = bstr_ptr(tempstr);
    }

    if (htp_parse_response_header_generic(connp, h, data, len) != HTP_OK) {
        if (tempstr != NULL) {
            free(tempstr);
        }
        free(h);
        return HTP_ERROR;
    }

    // Do we already have a header with the same name?
    htp_header_t *h_existing = table_get(connp->out_tx->response_headers, h->name);
    if (h_existing != NULL) {
        // TODO Do we want to keep a list of the headers that are
        //      allowed to be combined in this way?

        // Add to existing header
        h_existing->value = bstr_expand(h_existing->value, bstr_len(h_existing->value)
            + 2 + bstr_len(h->value));
        bstr_add_mem(h_existing->value, ", ", 2);
        bstr_add_str(h_existing->value, h->value);

        // The header is no longer needed
        free(h->name);
        free(h->value);
        free(h);

        // Keep track of same-name headers
        h_existing->flags |= HTP_FIELD_REPEATED;
    } else {
        // Add as a new header
        table_add(connp->out_tx->response_headers, h->name, h);
    }

    if (tempstr != NULL) {
        free(tempstr);
    }

    return HTP_OK;
}
