/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2017-2019 Fraunhofer IOSB (Author: Andreas Ebner)
 * Copyright (c) 2019 Fraunhofer IOSB (Author: Julius Pfrommer)
 * Copyright (c) 2019-2021 Kalycito Infotech Private Limited
 * Copyright (c) 2020 Yannick Wallerer, Siemens AG
 * Copyright (c) 2020 Thomas Fischer, Siemens AG
 * Copyright (c) 2021 Fraunhofer IOSB (Author: Jan Hermes)
 */

#include "ua_pubsub.h"
#include "server/ua_server_internal.h"

#ifdef UA_ENABLE_PUBSUB /* conditional compilation */

static void
UA_DataSetField_clear(UA_DataSetField *field) {
    UA_DataSetFieldConfig_clear(&field->config);
    UA_NodeId_clear(&field->identifier);
    UA_NodeId_clear(&field->publishedDataSet);
    UA_FieldMetaData_clear(&field->fieldMetaData);
}

UA_StatusCode
UA_PublishedDataSetConfig_copy(const UA_PublishedDataSetConfig *src,
                               UA_PublishedDataSetConfig *dst) {
    UA_StatusCode res = UA_STATUSCODE_GOOD;
    memcpy(dst, src, sizeof(UA_PublishedDataSetConfig));
    res |= UA_String_copy(&src->name, &dst->name);
    switch(src->publishedDataSetType) {
        case UA_PUBSUB_DATASET_PUBLISHEDITEMS:
            //no additional items
            break;

        case UA_PUBSUB_DATASET_PUBLISHEDITEMS_TEMPLATE:
            if(src->config.itemsTemplate.variablesToAddSize > 0) {
                dst->config.itemsTemplate.variablesToAdd = (UA_PublishedVariableDataType *)
                    UA_calloc(src->config.itemsTemplate.variablesToAddSize,
                              sizeof(UA_PublishedVariableDataType));
                if(!dst->config.itemsTemplate.variablesToAdd) {
                    res = UA_STATUSCODE_BADOUTOFMEMORY;
                    break;
                }
                dst->config.itemsTemplate.variablesToAddSize =
                    src->config.itemsTemplate.variablesToAddSize;
            }

            for(size_t i = 0; i < src->config.itemsTemplate.variablesToAddSize; i++) {
                res |= UA_PublishedVariableDataType_copy(&src->config.itemsTemplate.variablesToAdd[i],
                                                         &dst->config.itemsTemplate.variablesToAdd[i]);
            }
            res |= UA_DataSetMetaDataType_copy(&src->config.itemsTemplate.metaData,
                                               &dst->config.itemsTemplate.metaData);
            break;

        default:
            res = UA_STATUSCODE_BADINVALIDARGUMENT;
            break;
    }

    if(res != UA_STATUSCODE_GOOD)
        UA_PublishedDataSetConfig_clear(dst);
    return res;
}

UA_StatusCode
UA_Server_getPublishedDataSetConfig(UA_Server *server, const UA_NodeId pds,
                                    UA_PublishedDataSetConfig *config) {
    if(!config)
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    UA_PublishedDataSet *currentPDS = UA_PublishedDataSet_findPDSbyId(server, pds);
    if(!currentPDS)
        return UA_STATUSCODE_BADNOTFOUND;
    return UA_PublishedDataSetConfig_copy(&currentPDS->config, config);
}

UA_StatusCode
UA_Server_getPublishedDataSetMetaData(UA_Server *server, const UA_NodeId pds,
                                      UA_DataSetMetaDataType *metaData) {
    if(!metaData)
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    UA_PublishedDataSet *currentPDS = UA_PublishedDataSet_findPDSbyId(server, pds);
    if(!currentPDS)
        return UA_STATUSCODE_BADNOTFOUND;
    return UA_DataSetMetaDataType_copy(&currentPDS->dataSetMetaData, metaData);
}

UA_PublishedDataSet *
UA_PublishedDataSet_findPDSbyId(UA_Server *server, UA_NodeId identifier) {
    UA_PublishedDataSet *tmpPDS = NULL;
    TAILQ_FOREACH(tmpPDS, &server->pubSubManager.publishedDataSets, listEntry) {
        if(UA_NodeId_equal(&tmpPDS->identifier, &identifier))
            break;
    }
    return tmpPDS;
}

UA_PublishedDataSet *
UA_PublishedDataSet_findPDSbyName(UA_Server *server, UA_String name) {
    UA_PublishedDataSet *tmpPDS = NULL;
    TAILQ_FOREACH(tmpPDS, &server->pubSubManager.publishedDataSets, listEntry) {
        if(UA_String_equal(&name, &tmpPDS->config.name))
            break;
    }

    return tmpPDS;
}

void
UA_PublishedDataSetConfig_clear(UA_PublishedDataSetConfig *pdsConfig) {
    //delete pds config
    UA_String_clear(&pdsConfig->name);
    switch (pdsConfig->publishedDataSetType){
        case UA_PUBSUB_DATASET_PUBLISHEDITEMS:
            //no additional items
            break;
        case UA_PUBSUB_DATASET_PUBLISHEDITEMS_TEMPLATE:
            if(pdsConfig->config.itemsTemplate.variablesToAddSize > 0){
                for(size_t i = 0; i < pdsConfig->config.itemsTemplate.variablesToAddSize; i++){
                    UA_PublishedVariableDataType_clear(&pdsConfig->config.itemsTemplate.variablesToAdd[i]);
                }
                UA_free(pdsConfig->config.itemsTemplate.variablesToAdd);
            }
            UA_DataSetMetaDataType_clear(&pdsConfig->config.itemsTemplate.metaData);
            break;
        default:
            break;
    }
}

void
UA_PublishedDataSet_clear(UA_Server *server, UA_PublishedDataSet *publishedDataSet) {
    UA_DataSetField *field, *tmpField;
    TAILQ_FOREACH_SAFE(field, &publishedDataSet->fields, listEntry, tmpField) {
        removeDataSetField(server, field->identifier);
    }
    UA_PublishedDataSetConfig_clear(&publishedDataSet->config);
    UA_DataSetMetaDataType_clear(&publishedDataSet->dataSetMetaData);
    UA_NodeId_clear(&publishedDataSet->identifier);
}

/* The fieldMetaData variable has to be cleaned up external in case of an error */
static UA_StatusCode
generateFieldMetaData(UA_Server *server, UA_PublishedDataSet *pds,
                      UA_DataSetField *field, UA_FieldMetaData *fieldMetaData) {
    if(field->config.dataSetFieldType != UA_PUBSUB_DATASETFIELD_VARIABLE)
        return UA_STATUSCODE_BADNOTSUPPORTED;

    /* Set the field identifier */
    fieldMetaData->dataSetFieldId = UA_PubSubManager_generateUniqueGuid(server);

    /* Set the description */
    fieldMetaData->description = UA_LOCALIZEDTEXT_ALLOC("", "");

    /* Set the name */
    const UA_DataSetVariableConfig *var = &field->config.field.variable;
    UA_StatusCode res = UA_String_copy(&var->fieldNameAlias, &fieldMetaData->name);
    UA_CHECK_STATUS(res, return res);

    /* Static value source. ToDo after freeze PR, the value source must be
     * checked (other behavior for static value source) */
    if(var->rtValueSource.rtFieldSourceEnabled &&
       !var->rtValueSource.rtInformationModelNode) {
        const UA_DataValue *svs = *var->rtValueSource.staticValueSource;
        if(svs->value.arrayDimensionsSize > 0) {
            fieldMetaData->arrayDimensions = (UA_UInt32 *)
                UA_calloc(svs->value.arrayDimensionsSize, sizeof(UA_UInt32));
            if(fieldMetaData->arrayDimensions == NULL)
                return UA_STATUSCODE_BADOUTOFMEMORY;
            memcpy(fieldMetaData->arrayDimensions, svs->value.arrayDimensions,
                   sizeof(UA_UInt32) * svs->value.arrayDimensionsSize);
        }
        fieldMetaData->arrayDimensionsSize = svs->value.arrayDimensionsSize;

        res = UA_NodeId_copy(&svs->value.type->typeId, &fieldMetaData->dataType);
        UA_CHECK_STATUS(res, return res);

        //TODO collect value rank for the static field source
        fieldMetaData->properties = NULL;
        fieldMetaData->propertiesSize = 0;
        fieldMetaData->fieldFlags = UA_DATASETFIELDFLAGS_NONE;
        return UA_STATUSCODE_GOOD;
    }

    /* Set the Array Dimensions */
    const UA_PublishedVariableDataType *pp = &var->publishParameters;
    UA_Variant value;
    UA_Variant_init(&value);
    res = readWithReadValue(server, &pp->publishedVariable,
                            UA_ATTRIBUTEID_ARRAYDIMENSIONS, &value);
    if(res != UA_STATUSCODE_GOOD) {
        UA_LOG_WARNING_DATASET(&server->config.logger, pds,
                               "PubSub meta data generation: Reading the array dimensions failed");
        return res;
    }

    if(value.arrayDimensionsSize > 0) {
        fieldMetaData->arrayDimensions = (UA_UInt32 *)
            UA_calloc(value.arrayDimensionsSize, sizeof(UA_UInt32));
        if(!fieldMetaData->arrayDimensions)
            return UA_STATUSCODE_BADOUTOFMEMORY;
        memcpy(fieldMetaData->arrayDimensions, value.arrayDimensions,
               sizeof(UA_UInt32)*value.arrayDimensionsSize);
    }
    fieldMetaData->arrayDimensionsSize = value.arrayDimensionsSize;

    /* Set the DataType */
    res = readWithReadValue(server, &pp->publishedVariable,
                            UA_ATTRIBUTEID_DATATYPE, &fieldMetaData->dataType);
    if(res != UA_STATUSCODE_GOOD) {
        UA_LOG_WARNING_DATASET(&server->config.logger, pds,
                               "PubSub meta data generation: Reading the datatype failed");
        return res;
    }

    if(!UA_NodeId_isNull(&fieldMetaData->dataType)) {
        const UA_DataType *currentDataType =
            UA_findDataTypeWithCustom(&fieldMetaData->dataType,
                                      server->config.customDataTypes);
#ifdef UA_ENABLE_TYPEDESCRIPTION
        UA_LOG_DEBUG_DATASET(&server->config.logger, pds,
                             "MetaData creation: Found DataType %s",
                             currentDataType->typeName);
#endif
        /* Check if the datatype is a builtInType, if yes set the builtinType. */
        if(currentDataType->typeKind <= UA_DATATYPEKIND_ENUM)
            fieldMetaData->builtInType = (UA_Byte)currentDataType->typeKind;
        /* set the maxStringLength attribute */
        if(field->config.field.variable.maxStringLength != 0){
            if(currentDataType->typeKind == UA_DATATYPEKIND_BYTESTRING ||
            currentDataType->typeKind == UA_DATATYPEKIND_STRING ||
            currentDataType->typeKind == UA_DATATYPEKIND_LOCALIZEDTEXT) {
                fieldMetaData->maxStringLength = field->config.field.variable.maxStringLength;
            } else {
                UA_LOG_WARNING_DATASET(&server->config.logger, pds,
                                       "PubSub meta data generation: MaxStringLength with incompatible DataType configured.");
            }
        }
    } else {
        UA_LOG_WARNING_DATASET(&server->config.logger, pds,
                               "PubSub meta data generation: DataType is UA_NODEID_NULL");
    }

    /* Set the ValueRank */
    UA_Int32 valueRank;
    res = readWithReadValue(server, &pp->publishedVariable,
                            UA_ATTRIBUTEID_VALUERANK, &valueRank);
    if(res != UA_STATUSCODE_GOOD) {
        UA_LOG_WARNING_DATASET(&server->config.logger, pds,
                               "PubSub meta data generation: Reading the value rank failed");
        return res;
    }
    fieldMetaData->valueRank = valueRank;

    /* PromotedField? */
    if(var->promotedField)
        fieldMetaData->fieldFlags = UA_DATASETFIELDFLAGS_PROMOTEDFIELD;
    else
        fieldMetaData->fieldFlags = UA_DATASETFIELDFLAGS_NONE;

    /* Properties */
    fieldMetaData->properties = NULL;
    fieldMetaData->propertiesSize = 0;

    //TODO collect the following fields*/
    //fieldMetaData.builtInType
    //fieldMetaData.maxStringLength

    return UA_STATUSCODE_GOOD;
}

static UA_DataSetFieldResult
addDataSetField(UA_Server *server, const UA_NodeId publishedDataSet,
                const UA_DataSetFieldConfig *fieldConfig,
                UA_NodeId *fieldIdentifier) {
    UA_DataSetFieldResult result;
    memset(&result, 0, sizeof(UA_DataSetFieldResult));
    if(!fieldConfig) {
        result.result = UA_STATUSCODE_BADINVALIDARGUMENT;
        return result;
    }

    UA_PublishedDataSet *currDS =
        UA_PublishedDataSet_findPDSbyId(server, publishedDataSet);
    if(!currDS) {
        result.result = UA_STATUSCODE_BADNOTFOUND;
        return result;
    }

    if(currDS->configurationFrozen) {
        UA_LOG_WARNING_DATASET(&server->config.logger, currDS,
                               "Adding DataSetField failed: PublishedDataSet is frozen");
        result.result = UA_STATUSCODE_BADCONFIGURATIONERROR;
        return result;
    }

    if(currDS->config.publishedDataSetType != UA_PUBSUB_DATASET_PUBLISHEDITEMS) {
        result.result = UA_STATUSCODE_BADNOTIMPLEMENTED;
        return result;
    }

    UA_DataSetField *newField = (UA_DataSetField*)UA_calloc(1, sizeof(UA_DataSetField));
    if(!newField) {
        result.result = UA_STATUSCODE_BADINTERNALERROR;
        return result;
    }

    result.result = UA_DataSetFieldConfig_copy(fieldConfig, &newField->config);
    if(result.result != UA_STATUSCODE_GOOD) {
        UA_free(newField);
        return result;
    }

    result.result = UA_NodeId_copy(&currDS->identifier, &newField->publishedDataSet);
    if(result.result != UA_STATUSCODE_GOOD) {
        UA_DataSetFieldConfig_clear(&newField->config);
        UA_free(newField);
        return result;
    }

    /* Initialize the field metadata. Also generates a FieldId */
    UA_FieldMetaData fmd;
    UA_FieldMetaData_init(&fmd);
    result.result = generateFieldMetaData(server, currDS, newField, &fmd);
    if(result.result != UA_STATUSCODE_GOOD) {
        UA_FieldMetaData_clear(&fmd);
        UA_DataSetFieldConfig_clear(&newField->config);
        UA_NodeId_clear(&newField->publishedDataSet);
        UA_free(newField);
        return result;
    }

    /* Append to the metadata fields array. Point of last return. */
    result.result = UA_Array_append((void**)&currDS->dataSetMetaData.fields,
                                    &currDS->dataSetMetaData.fieldsSize,
                                    &fmd, &UA_TYPES[UA_TYPES_FIELDMETADATA]);
    if(result.result != UA_STATUSCODE_GOOD) {
        UA_FieldMetaData_clear(&fmd);
        UA_DataSetFieldConfig_clear(&newField->config);
        UA_NodeId_clear(&newField->publishedDataSet);
        UA_free(newField);
        return result;
    }

    /* Copy the identifier from the metadata. Cannot fail with a guid NodeId. */
    newField->identifier = UA_NODEID_GUID(1, fmd.dataSetFieldId);
    if(fieldIdentifier)
        UA_NodeId_copy(&newField->identifier, fieldIdentifier);

    /* Register the field. The order of DataSetFields should be the same in both
     * creating and publishing. So adding DataSetFields at the the end of the
     * DataSets using the TAILQ structure. */
    TAILQ_INSERT_TAIL(&currDS->fields, newField, listEntry);
    currDS->fieldSize++;

    if(newField->config.field.variable.promotedField)
        currDS->promotedFieldsCount++;

    /* The values of the metadata are "borrowed" in a mirrored structure in the
     * pds. Reset them after resizing the array. */
    size_t counter = 0;
    UA_DataSetField *dsf;
    TAILQ_FOREACH(dsf, &currDS->fields, listEntry) {
        dsf->fieldMetaData = currDS->dataSetMetaData.fields[counter++];
    }

    /* Update major version of parent published data set */
    currDS->dataSetMetaData.configurationVersion.majorVersion =
        UA_PubSubConfigurationVersionTimeDifference();

    result.configurationVersion.majorVersion =
        currDS->dataSetMetaData.configurationVersion.majorVersion;
    result.configurationVersion.minorVersion =
        currDS->dataSetMetaData.configurationVersion.minorVersion;
    return result;
}

UA_DataSetFieldResult
UA_Server_addDataSetField(UA_Server *server, const UA_NodeId publishedDataSet,
                          const UA_DataSetFieldConfig *fieldConfig,
                          UA_NodeId *fieldIdentifier) {
    UA_LOCK(&server->serviceMutex);
    UA_DataSetFieldResult res =
        addDataSetField(server, publishedDataSet, fieldConfig, fieldIdentifier);
    UA_UNLOCK(&server->serviceMutex);
    return res;
}

UA_DataSetFieldResult
removeDataSetField(UA_Server *server, const UA_NodeId dsf) {
    UA_DataSetFieldResult result;
    memset(&result, 0, sizeof(UA_DataSetFieldResult));

    UA_DataSetField *currentField = UA_DataSetField_findDSFbyId(server, dsf);
    if(!currentField) {
        result.result = UA_STATUSCODE_BADNOTFOUND;
        return result;
    }

    UA_PublishedDataSet *pds =
        UA_PublishedDataSet_findPDSbyId(server, currentField->publishedDataSet);
    if(!pds) {
        result.result = UA_STATUSCODE_BADNOTFOUND;
        return result;
    }

    if(currentField->configurationFrozen) {
        UA_LOG_WARNING_DATASET(&server->config.logger, pds,
                               "Remove DataSetField failed: DataSetField is frozen");
        result.result = UA_STATUSCODE_BADCONFIGURATIONERROR;
        return result;
    }

    if(pds->configurationFrozen) {
        UA_LOG_WARNING_DATASET(&server->config.logger, pds,
                               "Remove DataSetField failed: PublishedDataSet is frozen");
        result.result = UA_STATUSCODE_BADCONFIGURATIONERROR;
        return result;
    }

    /* Reduce the counters before the config is cleaned up */
    if(currentField->config.field.variable.promotedField)
        pds->promotedFieldsCount--;
    pds->fieldSize--;

    /* Update major version of PublishedDataSet */
    pds->dataSetMetaData.configurationVersion.majorVersion =
        UA_PubSubConfigurationVersionTimeDifference();

    /* Clean up */
    currentField->fieldMetaData.arrayDimensions = NULL;
    currentField->fieldMetaData.properties = NULL;
    currentField->fieldMetaData.name = UA_STRING_NULL;
    currentField->fieldMetaData.description.locale = UA_STRING_NULL;
    currentField->fieldMetaData.description.text = UA_STRING_NULL;
    UA_DataSetField_clear(currentField);

    /* Remove */
    TAILQ_REMOVE(&pds->fields, currentField, listEntry);
    UA_free(currentField);

    /* Regenerate DataSetMetaData */
    pds->dataSetMetaData.fieldsSize--;
    if(pds->dataSetMetaData.fieldsSize > 0) {
        for(size_t i = 0; i < pds->dataSetMetaData.fieldsSize+1; i++) {
            UA_FieldMetaData_clear(&pds->dataSetMetaData.fields[i]);
        }
        UA_free(pds->dataSetMetaData.fields);
        UA_FieldMetaData *fieldMetaData = (UA_FieldMetaData *)
            UA_calloc(pds->dataSetMetaData.fieldsSize, sizeof(UA_FieldMetaData));
        if(!fieldMetaData) {
            result.result =  UA_STATUSCODE_BADOUTOFMEMORY;
            return result;
        }
        UA_DataSetField *tmpDSF;
        size_t counter = 0;
        TAILQ_FOREACH(tmpDSF, &pds->fields, listEntry) {
            result.result = generateFieldMetaData(server, pds, tmpDSF, &fieldMetaData[counter]);
            if(result.result != UA_STATUSCODE_GOOD) {
                UA_FieldMetaData_clear(&fieldMetaData[counter]);
                UA_LOG_WARNING_DATASET(&server->config.logger, pds,
                                       "PubSub MetaData regeneration failed "
                                       "after removing a field!");
                break;
            }
            counter++;
        }
        pds->dataSetMetaData.fields = fieldMetaData;
    } else {
        UA_FieldMetaData_delete(pds->dataSetMetaData.fields);
        pds->dataSetMetaData.fields = NULL;
    }

    result.configurationVersion.majorVersion =
        pds->dataSetMetaData.configurationVersion.majorVersion;
    result.configurationVersion.minorVersion =
        pds->dataSetMetaData.configurationVersion.minorVersion;
    return result;
}

UA_DataSetFieldResult
UA_Server_removeDataSetField(UA_Server *server, const UA_NodeId dsf) {
    UA_LOCK(&server->serviceMutex);
    UA_DataSetFieldResult res = removeDataSetField(server, dsf);
    UA_UNLOCK(&server->serviceMutex);
    return res;
}

UA_StatusCode
UA_DataSetFieldConfig_copy(const UA_DataSetFieldConfig *src,
                           UA_DataSetFieldConfig *dst) {
    if(src->dataSetFieldType != UA_PUBSUB_DATASETFIELD_VARIABLE)
        return UA_STATUSCODE_BADNOTSUPPORTED;
    memcpy(dst, src, sizeof(UA_DataSetFieldConfig));
    UA_StatusCode res = UA_STATUSCODE_GOOD;
    res |= UA_String_copy(&src->field.variable.fieldNameAlias,
                          &dst->field.variable.fieldNameAlias);
    res |= UA_PublishedVariableDataType_copy(&src->field.variable.publishParameters,
                                             &dst->field.variable.publishParameters);
    if(res != UA_STATUSCODE_GOOD)
        UA_DataSetFieldConfig_clear(dst);
    return res;
}

UA_StatusCode
UA_Server_getDataSetFieldConfig(UA_Server *server, const UA_NodeId dsf,
                                UA_DataSetFieldConfig *config) {
    if(!config)
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    UA_DataSetField *currentDataSetField = UA_DataSetField_findDSFbyId(server, dsf);
    if(!currentDataSetField)
        return UA_STATUSCODE_BADNOTFOUND;
    return UA_DataSetFieldConfig_copy(&currentDataSetField->config, config);
}

UA_DataSetField *
UA_DataSetField_findDSFbyId(UA_Server *server, UA_NodeId identifier) {
    UA_PublishedDataSet *tmpPDS;
    TAILQ_FOREACH(tmpPDS, &server->pubSubManager.publishedDataSets, listEntry) {
        UA_DataSetField *tmpField;
        TAILQ_FOREACH(tmpField, &tmpPDS->fields, listEntry) {
            if(UA_NodeId_equal(&tmpField->identifier, &identifier))
                return tmpField;
        }
    }
    return NULL;
}

void
UA_DataSetFieldConfig_clear(UA_DataSetFieldConfig *dataSetFieldConfig) {
    if(dataSetFieldConfig->dataSetFieldType == UA_PUBSUB_DATASETFIELD_VARIABLE) {
        UA_String_clear(&dataSetFieldConfig->field.variable.fieldNameAlias);
        UA_PublishedVariableDataType_clear(&dataSetFieldConfig->field.variable.publishParameters);
    }
}

/* Obtain the latest value for a specific DataSetField. This method is currently
 * called inside the DataSetMessage generation process. */
void
UA_PubSubDataSetField_sampleValue(UA_Server *server, UA_DataSetField *field,
                                  UA_DataValue *value) {
    UA_PublishedVariableDataType *params = &field->config.field.variable.publishParameters;

    /* Read the value */
    if(field->config.field.variable.rtValueSource.rtInformationModelNode) {
        const UA_VariableNode *rtNode = (const UA_VariableNode *)
            UA_NODESTORE_GET(server, &params->publishedVariable);
        *value = **rtNode->valueBackend.backend.external.value;
        value->value.storageType = UA_VARIANT_DATA_NODELETE;
        UA_NODESTORE_RELEASE(server, (const UA_Node *) rtNode);
    } else if(field->config.field.variable.rtValueSource.rtFieldSourceEnabled == false){
        UA_ReadValueId rvid;
        UA_ReadValueId_init(&rvid);
        rvid.nodeId = params->publishedVariable;
        rvid.attributeId = params->attributeId;
        rvid.indexRange = params->indexRange;
        *value = readAttribute(server, &rvid, UA_TIMESTAMPSTORETURN_BOTH);
    } else {
        *value = **field->config.field.variable.rtValueSource.staticValueSource;
        value->value.storageType = UA_VARIANT_DATA_NODELETE;
    }
}

#endif /* UA_ENABLE_PUBSUB */
